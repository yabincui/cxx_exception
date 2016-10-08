#include "elf_reader.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "dwarf.h"
#include "dwarf_string.h"
#include "read_utils.h"

#define CHECK(expr) \
  if (!(expr)) \
    abort()

static void PrintHex(const char* p, uint64_t len) {
  for (uint64_t i = 0; i < len; ++i) {
    printf("%x ", (unsigned char)p[i]);
  }
  printf("\n");
}

static const char* FindMap(const std::unordered_map<int, const char*>& map, uint64_t key) {
  auto it = map.find(key);
  if (it != map.end()) {
    return it->second;
  }
  return "";
}

static const char* GetProgramHeaderType(int type) {
  static const std::unordered_map<int, const char*> map = {
    {PT_NULL, "PT_NULL"},
    {PT_LOAD, "PT_LOAD"},
    {PT_DYNAMIC, "PT_DYNAMIC"},
    {PT_INTERP, "PT_INTERP"},
    {PT_NOTE, "PT_NOTE"},
    {PT_SHLIB, "PT_SHLIB"},
    {PT_PHDR, "PT_PHDR"},
    {PT_TLS, "PT_TLS"},
  };
  auto it = map.find(type);
  if (it != map.end()) {
    return it->second;
  }
  return "?";
}

static std::string GetProgramHeaderFlags(int flags) {
  std::string result;
  if (flags & PF_X) {
    result.push_back('X');
  }
  if (flags & PF_W) {
    result.push_back('W');
  }
  if (flags & PF_R) {
    result.push_back('R');
  }
  return result;
}


struct Elf64Struct {
  using Elf_Ehdr = Elf64_Ehdr;
  using Elf_Shdr = Elf64_Shdr;
  using Elf_Phdr = Elf64_Phdr;
  static const int ELFCLASS = ELFCLASS64;
};

struct Elf32Struct {
  using Elf_Ehdr = Elf32_Ehdr;
  using Elf_Shdr = Elf32_Shdr;
  using Elf_Phdr = Elf32_Phdr;
  static const int ELFCLASS = ELFCLASS32;
};

template <typename ElfStruct>
class ElfReaderImpl : public ElfReader {
 public:
  using Elf_Ehdr = typename ElfStruct::Elf_Ehdr;
  using Elf_Shdr = typename ElfStruct::Elf_Shdr;
  using Elf_Phdr = typename ElfStruct::Elf_Phdr;

  ElfReaderImpl(FILE* fp, const char* filename, int log_flag)
      : ElfReader(filename),
        fp_(fp), fd_(fileno(fp_)), log_flag_(log_flag),
        read_section_flag_(0) {
  }

  virtual ~ElfReaderImpl() {
    fclose(fp_);
  }

  bool ReadEhFrame() override;

 protected:
  bool ReadHeader() override {
    if (!ReadFully(&header_, sizeof(header_), 0)) {
      return false;
    }
    if (memcmp(header_.e_ident, ELFMAG, SELFMAG) != 0) {
      fprintf(stderr, "elf magic doesn't match\n");
      return false;
    }
    int elf_class = header_.e_ident[EI_CLASS];
    if (elf_class != ElfStruct::ELFCLASS) {
      fprintf(stderr, "%s is %d-bit elf, doesn't match expected\n",
              filename_.c_str(), elf_class == ELFCLASS32 ? 32 : 64);
      return false;
    }
    if (log_flag_ & LOG_HEADER) {
      printf("section offset: %lx\n", (unsigned long)header_.e_shoff);
      printf("section num: %lx\n", (unsigned long)header_.e_shnum);
      printf("section entry size: %lx\n", (unsigned long)header_.e_shentsize);
      printf("string section index: %lu\n", (unsigned long)header_.e_shstrndx);
    }
    return true;
  }

  bool ReadSecHeaders() override {
    if (header_.e_shstrndx == 0) {
      fprintf(stderr, "string section is empty\n");
      return false;
    }
    Elf_Shdr str_sec;
    if (!ReadFully(&str_sec, sizeof(str_sec), header_.e_shoff + header_.e_shstrndx *
                   header_.e_shentsize)) {
      return false;
    }
    string_section_.resize(str_sec.sh_size);
    if (!ReadFully(string_section_.data(), str_sec.sh_size, str_sec.sh_offset)) {
      return false;
    }
    unsigned long offset = header_.e_shoff;
    for (int i = 0; i < header_.e_shnum; ++i, offset += header_.e_shentsize) {
      Elf_Shdr sec;
      if (!ReadFully(&sec, sizeof(sec), offset)) {
        return false;
      }
      const char* name = &string_section_[sec.sh_name];
      if (name[0] == '\0') {
        continue;
      }
      sec_headers_[name] = sec;
    }
    if (log_flag_ & LOG_SECTION_HEADERS) {
      for (auto& pair : sec_headers_) {
        printf("section %s, addr %lx, offset %lx, size %lx\n",
               pair.first.c_str(), (unsigned long)pair.second.sh_addr,
               (unsigned long)pair.second.sh_offset,
               (unsigned long)pair.second.sh_size);
      }
    }
    return true;
  }

  bool ReadProgramHeaders() override {
    unsigned long offset = header_.e_phoff;
    for (int i = 0; i < header_.e_phnum; ++i, offset += header_.e_phentsize) {
      Elf_Phdr ph;
      if (!ReadFully(&ph, sizeof(ph), offset)) {
        return false;
      }
      program_headers_.push_back(ph);
    }
    if (log_flag_ & LOG_PROGRAM_HEADERS) {
      for (const auto& ph : program_headers_) {
        printf("program header type %s(%lx) flag (%s)(%lx),offset %lx, vaddr %lx, paddr %lx, size %lx\n",
               GetProgramHeaderType(ph.p_type), (unsigned long)ph.p_type,
               GetProgramHeaderFlags(ph.p_flags).c_str(), (unsigned long)ph.p_flags,
               (unsigned long)ph.p_offset,
               (unsigned long)ph.p_vaddr, (unsigned long)ph.p_paddr,
               (unsigned long)ph.p_filesz);
      }
    }
    return true;
  }

  uint64_t ReadMinVirtualAddress() override {
    uint64_t min_vaddr = ULLONG_MAX;
    for (const auto& ph : program_headers_) {
      if (ph.p_type == PT_LOAD && ph.p_flags & PF_X) {
        if (min_vaddr > ph.p_vaddr) {
          min_vaddr = ph.p_vaddr;
        }
      }
    }
    return min_vaddr;
  }

 private:
  bool ReadFully(void* buf, size_t size, size_t offset) {
    ssize_t rc = TEMP_FAILURE_RETRY(pread64(fd_, buf, size, offset));
    if (rc < 0) {
      fprintf(stderr, "failed to read file: %s\n", strerror(errno));
      return false;
    }
    if (rc != size) {
      fprintf(stderr, "not read fully\n");
      return false;
    }
    return true;
  }

  const Elf_Shdr* GetSection(const char* name) {
    auto it = sec_headers_.find(name);
    if (it != sec_headers_.end()) {
      return &it->second;
    }
    fprintf(stderr, "No %s section in %s\n", name, filename_.c_str());
    return nullptr;
  }

  std::vector<char> ReadSection(const Elf_Shdr* section) {
    std::vector<char> data(section->sh_size);
    if (!ReadFully(data.data(), data.size(), section->sh_offset)) {
      return std::vector<char>();
    }
    return data;
  }

  FILE* fp_;
  int fd_;
  int log_flag_;
  int read_section_flag_;
  Elf_Ehdr header_;
  std::map<std::string, Elf_Shdr> sec_headers_;
  std::vector<char> string_section_;
  std::vector<Elf_Phdr> program_headers_;
};

template <typename ElfStruct>
bool ElfReaderImpl<ElfStruct>::ReadEhFrame() {
  if (read_section_flag_ & READ_EH_FRAME_SECTION) {
    return true;
  }
  const Elf_Shdr* eh_frame_sec = GetSection(".eh_frame");
  if (eh_frame_sec == nullptr) {
    return false;
  }
  std::vector<char> eh_frame_data = ReadSection(eh_frame_sec);
  const char* begin = eh_frame_data.data();
  const char* end = begin + eh_frame_data.size();
  const char* p;
  bool is_eh_frame = true;
  if (log_flag_ & LOG_EH_FRAME_SECTION) {
    printf(".eh_frame:\n");
    CieTable cie_table;
    for (p = begin; p < end;) {
      const char* cie_begin = p;
      bool section64 = false;
      int secbytes = 4;
      uint64_t unit_len = 0;
      uint32_t len = Read(p, 4);
      if (len == 0xffffffff) {
        section64 = true;
        secbytes = 8;
        unit_len = Read(p, 8);
      } else {
        unit_len = len;
      }
      if (unit_len == 0) {
        printf("<%lx> zero terminator\n", cie_begin - begin);
        continue;
      }
      const char* cie_end = p + unit_len;
      uint64_t cie_id = Read(p, secbytes);
      if (!section64 && cie_id == DW_CIE_ID_32) {
        cie_id = DW_CIE_ID_64;
      }
      bool is_cie = (is_eh_frame ? cie_id == 0 : cie_id == DW_CIE_ID_64);
      printf("\n<%lx> cie_id %" PRIx64 " %s\n", cie_begin - begin, cie_id, is_cie ? "CIE" : "FDE");
      Cie* cie = nullptr;
      if (is_cie) {
        cie = cie_table.CreateCie(cie_begin - begin);
        uint8_t version = Read(p, 1);
        printf("version %u\n", version);
        const char* augmentation = ReadStr(p);
        cie->augmentation = augmentation;
        printf("augmentation %s\n", augmentation);
        CHECK(augmentation[0] == '\0' || augmentation[0] == 'z');
        uint8_t address_size = 8; // ELF32 or ELF64
        if (version >= 4) {
          address_size = Read(p, 1);
          uint8_t segment_size = Read(p, 1);
          printf("address_size %d, segment_size %d\n", address_size, segment_size);
        }
        cie->address_size = address_size;
        uint64_t code_alignment_factor = ReadULEB128(p);
        int64_t data_alignment_factor = ReadLEB128(p);
        printf("code_alignment_factor %" PRIu64 ", data_alignment_factor %" PRId64 "\n",
               code_alignment_factor, data_alignment_factor);
        cie->data_alignment_factor = data_alignment_factor;
        uint64_t return_address_register;
        if (version == 1) {
          return_address_register = Read(p, 1);
        } else {
          return_address_register = ReadULEB128(p);
        }
        printf("return_address_register %" PRIu64 "\n", return_address_register);
        if (augmentation[0] == 'z') {
          uint64_t augmentation_len = ReadULEB128(p);
          printf("augmentation_len %" PRIu64 "\n", augmentation_len);
          for (int i = 1; augmentation[i] != '\0'; ++i) {
            char c = augmentation[i];
            if (c == 'R') {
              uint8_t fde_pointer_encoding = Read(p, 1);
              cie->fde_pointer_encoding = fde_pointer_encoding;
              printf("fde_pointer_encoding %x\n", fde_pointer_encoding);
            } else if (c == 'P') {
              uint8_t encoding = Read(p, 1);
              const char* encoding_str = FindMap(DWARF_EH_ENCODING_MAP, encoding);
              printf("personality pointer encoding 0x%x (%s)\n", encoding, encoding_str);
              uint64_t personality_handler = ReadEhEncoding(p, encoding);
              printf("personality pointer 0x%" PRIx64 "\n", personality_handler);
            } else if (c == 'L') {
              uint8_t lsda_encoding = Read(p, 1);
              cie->lsda_encoding = lsda_encoding;
              const char* encoding_str = FindMap(DWARF_EH_ENCODING_MAP, lsda_encoding);
              printf("lsda_encoding 0x%x (%s)\n", lsda_encoding, encoding_str);
            } else {
              fprintf(stderr, "unexpected augmentation %c\n", c);
              abort();
            }
          }
        }
        // initial_instructions
        printf("initial_instructions len 0x%lx\n", cie_end - p);
      } else {
        uint64_t cie_offset = (is_eh_frame ? p - secbytes - begin - cie_id : cie_id);
        printf("cie_offset 0x%" PRIx64 "\n", cie_offset);
        cie = cie_table.FindCie(cie_offset);
        if (cie == nullptr) {
          return false;
        }
        const char* base = p;
        uint64_t initial_location = ReadEhEncoding(p, cie->fde_pointer_encoding);
        uint64_t address_range = ReadEhEncoding(p, cie->fde_pointer_encoding);
        printf("initial_location 0x%" PRIx64 ", address_range 0x%" PRIx64"\n",
               initial_location, address_range);
        uint64_t proc_start = initial_location;
        if ((cie->fde_pointer_encoding & 0x70) == DW_EH_PE_pcrel) {
          proc_start += eh_frame_sec->sh_addr + (base - begin);
        }
        printf("proc range [0x%" PRIx64 " - 0x%" PRIx64 "]\n", proc_start, proc_start + address_range);
        if (cie->augmentation[0] == 'z') {
          uint64_t augmentation_len = ReadULEB128(p);
          printf("augmentation_len %" PRIu64 "\n", augmentation_len);
        }
        if (cie->lsda_encoding) {
          uint64_t lsda = ReadEhEncoding(p, cie->lsda_encoding);
          printf("lsda 0x%" PRIx64 "\n", lsda);
        }
        printf("instructions len 0x%lx\n", cie_end - p);
      }
      p = cie_end;
    }
  }

  for (p = begin; p < end;) {
    const char* cie_begin = p;
    bool section64 = false;
    int secbytes = 4;
    uint64_t unit_len = 0;
    uint32_t len = Read(p, 4);
    if (len == 0xffffffff) {
      section64 = true;
      secbytes = 8;
      unit_len = Read(p, 8);
    } else {
      unit_len = len;
    }
    if (unit_len == 0) {
      continue;
    }
    const char* cie_end = p + unit_len;
    uint64_t cie_id = Read(p, secbytes);
    if (!section64 && cie_id == DW_CIE_ID_32) {
      cie_id = DW_CIE_ID_64;
    }
    bool is_cie = (is_eh_frame ? cie_id == 0 : cie_id == DW_CIE_ID_64);
    Cie* cie = nullptr;
    if (is_cie) {
      cie = cie_table_.CreateCie(cie_begin - begin);
      cie->section64 = section64;
      uint8_t version = Read(p, 1);
      const char* augmentation = ReadStr(p);
      cie->augmentation = augmentation;
      CHECK(augmentation[0] == '\0' || augmentation[0] == 'z');
      uint8_t address_size = 8; // ELF32 or ELF64
      if (version >= 4) {
        address_size = Read(p, 1);
        uint8_t segment_size = Read(p, 1);
      }
      cie->address_size = address_size;
      uint64_t code_alignment_factor = ReadULEB128(p);
      int64_t data_alignment_factor = ReadLEB128(p);
      cie->data_alignment_factor = data_alignment_factor;
      uint64_t return_address_register;
      if (version == 1) {
        return_address_register = Read(p, 1);
      } else {
        return_address_register = ReadULEB128(p);
      }
      if (augmentation[0] == 'z') {
        uint64_t augmentation_len = ReadULEB128(p);
        for (int i = 1; augmentation[i] != '\0'; ++i) {
          char c = augmentation[i];
          if (c == 'R') {
            uint8_t fde_pointer_encoding = Read(p, 1);
            cie->fde_pointer_encoding = fde_pointer_encoding;
          } else if (c == 'P') {
            uint8_t encoding = Read(p, 1);
            uint64_t personality_handler = ReadEhEncoding(p, encoding);
          } else if (c == 'L') {
            uint8_t lsda_encoding = Read(p, 1);
            cie->lsda_encoding = lsda_encoding;
          } else {
            fprintf(stderr, "unexpected augmentation %c\n", c);
            abort();
          }
        }
      }
      // initial_instructions
      cie->insts.insert(cie->insts.begin(), p, cie_end);
    } else {
      uint64_t cie_offset = (is_eh_frame ? p - secbytes - begin - cie_id : cie_id);
      cie = cie_table_.FindCie(cie_offset);
      if (cie == nullptr) {
        return false;
      }
      const char* base = p;
      uint64_t initial_location = ReadEhEncoding(p, cie->fde_pointer_encoding);
      uint64_t address_range = ReadEhEncoding(p, cie->fde_pointer_encoding);
      uint64_t proc_start = initial_location;
      if ((cie->fde_pointer_encoding & 0x70) == DW_EH_PE_pcrel) {
        proc_start += eh_frame_sec->sh_addr + (base - begin);
      }
      Fde* fde = fde_table_.CreateFde(proc_start);
      fde->cie = cie;
      fde->section64 = section64;
      fde->func_start = proc_start;
      fde->func_end = proc_start + address_range;
      if (cie->augmentation[0] == 'z') {
        uint64_t augmentation_len = ReadULEB128(p);
      }
      if (cie->lsda_encoding) {
        uint64_t lsda = ReadEhEncoding(p, cie->lsda_encoding);
      }
      fde->insts.insert(fde->insts.begin(), p, cie_end);
    }
    p = cie_end;
  }
  read_section_flag_ |= READ_EH_FRAME_SECTION;
  return true;
}

std::unique_ptr<ElfReader> ElfReader::Create(const char* filename, int log_flag) {
  FILE* fp = fopen(filename, "rb");
  if (fp == nullptr) {
    fprintf(stderr, "failed to open %s\n", filename);
    return nullptr;
  }
  char buf[EI_NIDENT];
  if (fread(buf, sizeof(buf), 1, fp) != 1) {
    fprintf(stderr, "failed to read %s\n", filename);
    return nullptr;
  }
  if (memcmp(buf, ELFMAG, SELFMAG) != 0) {
    fprintf(stderr, "elf magic doesn't match\n");
    return nullptr;
  }
  int elf_class = buf[EI_CLASS];
  std::unique_ptr<ElfReader> result;
  if (elf_class == ELFCLASS64) {
    result.reset(new ElfReaderImpl<Elf64Struct>(fp, filename, log_flag));
  } else if (elf_class == ELFCLASS32) {
    result.reset(new ElfReaderImpl<Elf32Struct>(fp, filename, log_flag));
  } else {
    fprintf(stderr, "wrong elf class\n");
    return nullptr;
  }
  if (!result->ReadHeader() || !result->ReadSecHeaders() ||
      !result->ReadProgramHeaders()) {
    return nullptr;
  }
  result->ReadMinVaddr();
  return result;
}

std::unordered_map<std::string, std::unique_ptr<ElfReader>>& ElfReaderManager::reader_table_ =
    *new std::unordered_map<std::string, std::unique_ptr<ElfReader>>;

ElfReader* ElfReaderManager::OpenElf(const std::string& filename) {
  auto it = reader_table_.find(filename);
  if (it != reader_table_.end()) {
    return it->second.get();
  }
  reader_table_[filename] = ElfReader::Create(filename.c_str(), -1);
  return reader_table_[filename].get();
}