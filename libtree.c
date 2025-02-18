#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ctype.h>
#include <glob.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#define VERSION "3.0.0-dev"

#define ET_EXEC 2
#define ET_DYN 3

#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_STRTAB 5
#define DT_SONAME 14
#define DT_RPATH 15
#define DT_RUNPATH 29

#define ERR_INVALID_MAGIC 1
#define ERR_INVALID_CLASS 2
#define ERR_INVALID_DATA 3
#define ERR_INVALID_HEADER 4
#define ERR_INVALID_BITS 5
#define ERR_UNSUPPORTED_ELF_FILE 6
#define ERR_NO_EXEC_OR_DYN 7
#define ERR_INVALID_PHOFF 8
#define ERR_INVALID_PROG_HEADER 9
#define ERR_CANT_STAT 10
#define ERR_INVALID_DYNAMIC_SECTION 11
#define ERR_INVALID_DYNAMIC_ARRAY_ENTRY 12
#define ERR_NO_STRTAB 13
#define ERR_INVALID_SONAME 14
#define ERR_INVALID_RPATH 15
#define ERR_INVALID_RUNPATH 16
#define ERR_INVALID_NEEDED 17
#define ERR_NOT_FOUND 18
#define ERR_NO_PT_LOAD 19
#define ERR_VADDRS_NOT_ORDERED 20

#define DT_FLAGS_1 0x6ffffffb
#define DT_1_NODEFLIB 0x800

#define MAX_OFFSET_T 0xFFFFFFFFFFFFFFFF

#define REGULAR_RED "\033[0;31m"
#define BOLD_RED "\033[1;31m"
#define CLEAR "\033[0m"
#define BOLD_YELLOW "\033[33m"
#define BOLD_CYAN "\033[1;36m"
#define REGULAR_CYAN "\033[0;36m"
#define REGULAR_MAGENTA "\033[0;35m"
#define REGULAR_BLUE "\033[0;34m"
#define BRIGHT_BLACK "\033[0;90m"
#define REGULAR "\033[0m"

// don't judge me.
#define LIGHT_HORIZONTAL "\xe2\x94\x80"
#define LIGHT_QUADRUPLE_DASH_VERTICAL "\xe2\x94\x8a"
#define LIGHT_UP_AND_RIGHT "\xe2\x94\x94"
#define LIGHT_VERTICAL "\xe2\x94\x82"
#define LIGHT_VERTICAL_AND_RIGHT "\xe2\x94\x9c"

#define JUST_INDENT "    "
#define LIGHT_VERTICAL_WITH_INDENT LIGHT_VERTICAL "   "

#define SMALL_VEC_SIZE 16
#define MAX_RECURSION_DEPTH 32

// Libraries we do not show by default -- this reduces the verbosity quite a
// bit.
char const *exclude_list[] = {
    "libc.so",      "libpthread.so",      "libm.so",  "libgcc_s.so",
    "libstdc++.so", "ld-linux-x86-64.so", "libdl.so", "libc.musl-x86_64.so"};

struct header_64_t {
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct header_32_t {
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct prog_64_t {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct prog_32_t {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};

struct dyn_64_t {
    int64_t d_tag;
    uint64_t d_val;
};

struct dyn_32_t {
    int32_t d_tag;
    uint32_t d_val;
};

typedef enum { EITHER, BITS32, BITS64 } elf_bits_t;

typedef enum {
    INPUT,
    DIRECT,
    RPATH,
    LD_LIBRARY_PATH,
    RUNPATH,
    LD_SO_CONF,
    DEFAULT
} how_t;

struct found_t {
    how_t how;
    // only set when found by in the rpath NOT of the direct parent.  so, when
    // it is found in a "special" way only rpaths allow, which is worth
    // informing the user about.
    size_t depth;
};

// large buffer in which to copy rpaths, needed libraries and sonames.
struct string_table_t {
    char *arr;
    size_t n;
    size_t capacity;
};

struct visited_file_t {
    dev_t st_dev;
    ino_t st_ino;
};

struct visited_file_array_t {
    struct visited_file_t *arr;
    size_t n;
    size_t capacity;
};

struct libtree_state_t {
    int verbosity;
    int path;
    int color;

    struct string_table_t string_table;
    struct visited_file_array_t visited;

    // rpath substitutions values (note: OSNAME/OSREL are FreeBSD specific, LIB
    // is glibc/Linux specific -- we substitute all so we can support
    // cross-compiled binaries).
    char *PLATFORM;
    char *LIB;
    char *OSNAME;
    char *OSREL;

    // rpath stack: if lib_a needs lib_b needs lib_c and all have rpaths
    // then first lib_c's rpaths are considered, then lib_b's, then lib_a's.
    // so this data structure keeps a list of offsets into the string buffer
    // where rpaths start, like [lib_a_rpath_offset, lib_b_rpath_offset,
    // lib_c_rpath_offset]...
    size_t rpath_offsets[MAX_RECURSION_DEPTH];
    size_t ld_library_path_offset;
    size_t default_paths_offset;
    size_t ld_so_conf_offset;

    // This is so we know we have to print a | or white space
    // in the tree
    char found_all_needed[MAX_RECURSION_DEPTH];
};

// Keep track of the files we've see

/**
 * small_vec_u64 is an array that lives on the stack until it grows to the heap
 */
struct small_vec_u64_t {
    uint64_t buf[SMALL_VEC_SIZE];
    uint64_t *p;
    size_t n;
    size_t capacity;
};

static inline void utoa(char *str, size_t v) {
    char *p = str;
    do {
        *p++ = '0' + (v % 10);
        v /= 10;
    } while (v > 0);
    size_t len = p - str;
    for (size_t i = 0; i < len / 2; i++) {
        char tmp = str[i];
        str[i] = str[len - i - 1];
        str[len - i - 1] = tmp;
    }
    str[len] = '\0';
}

static inline void small_vec_u64_init(struct small_vec_u64_t *v) {
    memset(v, 0, sizeof(*v));
    v->p = v->buf;
}

static void small_vec_u64_append(struct small_vec_u64_t *v, uint64_t val) {
    // The hopefully likely path
    if (v->n < SMALL_VEC_SIZE) {
        v->p[v->n++] = val;
        return;
    }

    // The slow fallback on the heap
    if (v->n == SMALL_VEC_SIZE) {
        v->capacity = 2 * SMALL_VEC_SIZE;
        v->p = malloc(v->capacity * sizeof(uint64_t));
        if (v->p == NULL)
            exit(1);
        memcpy(v->p, v->buf, SMALL_VEC_SIZE * sizeof(uint64_t));
    } else if (v->n == v->capacity) {
        v->capacity *= 2;
        uint64_t *p = realloc(v->p, v->capacity * sizeof(uint64_t));
        if (p == NULL)
            exit(1);
        v->p = p;
    }

    v->p[v->n++] = val;
}

static void small_vec_u64_free(struct small_vec_u64_t *v) {
    if (v->n <= SMALL_VEC_SIZE)
        return;
    free(v->p);
    v->p = NULL;
}

/**
 * end of small_vec_u64_t
 */

static inline int host_is_little_endian() {
    int test = 1;
    char *bytes = (char *)&test;
    return bytes[0] == 1;
}

static int is_ascending_order(uint64_t *v, size_t n) {
    for (size_t j = 1; j < n; ++j)
        if (v[j - 1] >= v[j])
            return 0;

    return 1;
}

static void string_table_maybe_grow(struct string_table_t *t, size_t n) {
    // The likely case of not having to resize
    if (t->n + n <= t->capacity)
        return;

    // Otherwise give twice the amount of required space.
    t->capacity = 2 * (t->n + n);
    char *arr = realloc(t->arr, t->capacity);
    if (arr == NULL) {
        exit(1);
    }
    t->arr = arr;
}

static void string_table_store(struct string_table_t *t, char const *str) {
    size_t n = strlen(str) + 1;
    string_table_maybe_grow(t, n);
    memcpy(t->arr + t->n, str, n);
    t->n += n;
}

static void string_table_copy_from_file(struct string_table_t *t, FILE *fptr) {
    char c;
    // TODO: this could be a bit more efficient...
    while ((c = getc(fptr)) != '\0' && c != EOF) {
        string_table_maybe_grow(t, 1);
        t->arr[t->n++] = c;
    }
    string_table_maybe_grow(t, 1);
    t->arr[t->n++] = '\0';
}

static int is_in_exclude_list(char *soname) {
    // Get to the end.
    char *start = soname;
    char *end = strrchr(start, '\0');

    // Empty needed string, is that even possible?
    if (start == end)
        return 0;

    --end;

    // Strip "1234567890." from the right.
    while (end != start && ((*end >= '0' && *end <= '9') || *end == '.')) {
        --end;
    }

    // Check if we should skip this one.
    for (size_t j = 0; j < sizeof(exclude_list) / sizeof(char *); ++j) {
        size_t len = strlen(exclude_list[j]);
        if (strncmp(start, exclude_list[j], len) != 0)
            continue;
        return 1;
    }

    return 0;
}

static void tree_preamble(struct libtree_state_t *s, size_t depth) {
    if (depth == 0)
        return;

    for (size_t i = 0; i < depth - 1; ++i)
        fputs(s->found_all_needed[i] ? JUST_INDENT : LIGHT_VERTICAL_WITH_INDENT,
              stdout);

    fputs(s->found_all_needed[depth - 1]
              ? LIGHT_UP_AND_RIGHT LIGHT_HORIZONTAL LIGHT_HORIZONTAL " "
              : LIGHT_VERTICAL_AND_RIGHT LIGHT_HORIZONTAL LIGHT_HORIZONTAL " ",
          stdout);
}

static int recurse(char *current_file, size_t depth,
                   struct libtree_state_t *state, elf_bits_t bits,
                   struct found_t reason);

static void check_search_paths(struct found_t reason, size_t offset,
                               size_t *needed_not_found,
                               struct small_vec_u64_t *needed_buf_offsets,
                               size_t depth, struct libtree_state_t *s,
                               elf_bits_t bits) {
    char path[4096];
    char *path_end = path + 4096;

    char const *buf = s->string_table.arr;

    while (buf[offset] != '\0') {
        // First remove trailing colons
        while (buf[offset] == ':' && buf[offset] != '\0')
            ++offset;

        // Check if it was only colons
        if (buf[offset] == '\0')
            return;

        // Copy the search path until the first \0 or :
        char *dest = path;
        while (buf[offset] != '\0' && buf[offset] != ':' && dest != path_end)
            *dest++ = buf[offset++];

        // Path too long... Can't handle.
        if (dest + 1 >= path_end)
            continue;

        // Add a separator if necessary
        if (*(dest - 1) != '/')
            *dest++ = '/';

        // Keep track of the end of the current search path.
        char *search_path_end = dest;

        // Try to open it -- if we've found anything, swap it with the back.
        for (size_t i = 0; i < *needed_not_found;) {
            size_t soname_len = strlen(buf + needed_buf_offsets->p[i]);

            // Path too long, can't handle.
            if (search_path_end + soname_len + 1 >= path_end)
                continue;

            // Otherwise append.
            memcpy(search_path_end, buf + needed_buf_offsets->p[i],
                   soname_len + 1);
            s->found_all_needed[depth] = *needed_not_found <= 1;

            // And try to locate the lib.
            if (recurse(path, depth + 1, s, bits, reason) == 0) {
                // Found it, so swap out the current soname to the back,
                // and reduce the number of to be found by one.
                size_t tmp = needed_buf_offsets->p[i];
                needed_buf_offsets->p[i] =
                    needed_buf_offsets->p[*needed_not_found - 1];
                needed_buf_offsets->p[--(*needed_not_found)] = tmp;
            } else {
                ++i;
            }
        }
    }
}

static int interpolate_variables(struct libtree_state_t *s, size_t src,
                                 char const *ORIGIN) {
    // We do not write to dst if there is no variables to interpolate.
    size_t prev_src = src;
    size_t curr_src = src;

    struct string_table_t *st = &s->string_table;

    while (1) {
        // Find the next potential variable.
        char *dollar = strchr(st->arr + curr_src, '$');
        if (dollar == NULL)
            break;
        curr_src = dollar - st->arr;

        size_t bytes_to_dollar = curr_src - prev_src;

        // Go past the dollar.
        ++curr_src;

        // Remember if we have to look for matching curly braces.
        int curly = 0;
        if (st->arr[curr_src] == '{') {
            curly = 1;
            ++curr_src;
        }

        // String to interpolate.
        char const *var_val = NULL;
        if (strncmp(&st->arr[curr_src], "ORIGIN", 6) == 0) {
            var_val = ORIGIN;
            curr_src += 6;
        } else if (strncmp(&st->arr[curr_src], "LIB", 3) == 0) {
            var_val = s->LIB;
            curr_src += 3;
        } else if (strncmp(&st->arr[curr_src], "PLATFORM", 8) == 0) {
            var_val = s->PLATFORM;
            curr_src += 8;
        } else if (strncmp(&st->arr[curr_src], "OSNAME", 6) == 0) {
            var_val = s->OSNAME;
            curr_src += 6;
        } else if (strncmp(&st->arr[curr_src], "OSREL", 5) == 0) {
            var_val = s->OSREL;
            curr_src += 5;
        } else {
            continue;
        }

        // Require matching {...}.
        if (curly) {
            if (st->arr[curr_src] != '}') {
                continue;
            }
            ++curr_src;
        }

        size_t var_len = strlen(var_val);

        // Make sure we have enough space to write to.
        string_table_maybe_grow(st, bytes_to_dollar + var_len);

        // First copy over the string until the variable.
        memcpy(&st->arr[s->string_table.n], &st->arr[prev_src],
               bytes_to_dollar);
        s->string_table.n += bytes_to_dollar;

        // Then move prev_src until after the variable.
        prev_src = curr_src;

        // Then copy the variable value (without null).
        memcpy(&st->arr[s->string_table.n], var_val, var_len);
        s->string_table.n += var_len;
    }

    // Did we copy anything? That implies a variable was interpolated.
    // Copy the remainder, including the \0.
    if (prev_src != src) {
        string_table_store(st, st->arr + prev_src);
        return 1;
    }

    return 0;
}

static void print_colon_delimited_paths(char const *start, char const *indent) {
    while (1) {
        // Don't print empty string
        if (*start == '\0')
            break;

        // Find the next delimiter after start
        char *next = strchr(start, ':');

        // Don't print empty strings
        if (start == next) {
            ++start;
            continue;
        }

        fputs(indent, stdout);
        fputs(JUST_INDENT, stdout);

        // Print up to but not including : or \0, followed by a newline.
        if (next == NULL) {
            puts(start);
        } else {
            fwrite(start, 1, next - start, stdout);
            putchar('\n');
        }

        // We done yet?
        if (next == NULL)
            break;

        // Otherwise put the : back in place and continue.
        start = next + 1;
    }
}

static void print_line(size_t depth, char *name, char *color_bold,
                       char *color_regular, int highlight,
                       struct found_t reason, struct libtree_state_t *s) {
    tree_preamble(s, depth);
    // Color the filename different than the path name, if we have a path.
    char *slash = NULL;
    if (s->color && highlight && (slash = strrchr(name, '/')) != NULL) {
        fputs(color_regular, stdout);
        fwrite(name, 1, slash + 1 - name, stdout);
        fputs(color_bold, stdout);
        fputs(slash + 1, stdout);
    } else {
        if (s->color)
            fputs(color_bold, stdout);

        fputs(name, stdout);
    }
    if (s->color && highlight)
        fputs(CLEAR " " BOLD_YELLOW, stdout);
    else
        putchar(' ');
    switch (reason.how) {
    case RPATH:
        if (reason.depth + 1 >= depth) {
            fputs("[rpath]", stdout);
        } else {
            char num[8];
            utoa(num, reason.depth + 1);
            fputs("[rpath of ", stdout);
            fputs(num, stdout);
            putchar(']');
        }
        break;
    case LD_LIBRARY_PATH:
        fputs("[LD_LIBRARY_PATH]", stdout);
        break;
    case RUNPATH:
        fputs("[runpath]", stdout);
        break;
    case LD_SO_CONF:
        fputs("[ld.so.conf]", stdout);
        break;
    case DIRECT:
        fputs("[direct]", stdout);
        break;
    case DEFAULT:
        fputs("[default path]", stdout);
        break;
    default:
        break;
    }
    if (s->color)
        fputs(CLEAR "\n", stdout);
    else
        putchar('\n');
}

static void print_error(size_t depth, size_t needed_not_found,
                        struct small_vec_u64_t *needed_buf_offsets,
                        char *runpath, struct libtree_state_t *s,
                        int no_def_lib) {
    for (size_t i = 0; i < needed_not_found; ++i) {
        s->found_all_needed[depth] = i + 1 >= needed_not_found;
        tree_preamble(s, depth + 1);
        if (s->color)
            fputs(BOLD_RED, stdout);
        fputs(s->string_table.arr + needed_buf_offsets->p[i], stdout);
        fputs(s->color ? " not found" CLEAR "\n" : " not found\n", stdout);
    }

    // If anything was not found, we print the search paths in order they
    // are considered.
    char *box_vertical =
        s->color ? JUST_INDENT REGULAR_RED LIGHT_QUADRUPLE_DASH_VERTICAL CLEAR
                 : JUST_INDENT LIGHT_QUADRUPLE_DASH_VERTICAL;
    char *indent = malloc(sizeof(LIGHT_VERTICAL_WITH_INDENT) * depth +
                          strlen(box_vertical) + 1);
    char *p = indent;
    for (int i = 0; i < depth; ++i) {
        if (s->found_all_needed[i]) {
            int len = sizeof(JUST_INDENT) - 1;
            memcpy(p, JUST_INDENT, len);
            p += len;
        } else {
            int len = sizeof(LIGHT_VERTICAL_WITH_INDENT) - 1;
            memcpy(p, LIGHT_VERTICAL_WITH_INDENT, len);
            p += len;
        }
    }
    // dotted | in red
    strcpy(p, box_vertical);

    fputs(indent, stdout);
    if (s->color)
        fputs(BRIGHT_BLACK, stdout);
    fputs(" Paths considered in this order:\n", stdout);
    if (s->color)
        fputs(CLEAR, stdout);

    // Consider rpaths only when runpath is empty
    fputs(indent, stdout);
    if (runpath != NULL) {
        if (s->color)
            fputs(BRIGHT_BLACK, stdout);
        fputs(" 1. rpath is skipped because runpath was set\n", stdout);
        if (s->color)
            fputs(CLEAR, stdout);
    } else {
        fputs(s->color ? BRIGHT_BLACK " 1. rpath:" CLEAR "\n" : " 1. rpath:\n",
              stdout);
        for (int j = depth; j >= 0; --j) {
            if (s->rpath_offsets[j] != SIZE_MAX) {
                char num[8];
                utoa(num, j + 1);
                fputs(indent, stdout);
                if (s->color)
                    fputs(BRIGHT_BLACK, stdout);
                fputs("    depth ", stdout);
                fputs(num, stdout);
                if (s->color)
                    fputs(CLEAR, stdout);
                putchar('\n');
                print_colon_delimited_paths(
                    s->string_table.arr + s->rpath_offsets[j], indent);
            }
        }
    }

    fputs(indent, stdout);
    if (s->ld_library_path_offset == SIZE_MAX) {
        fputs(s->color ? BRIGHT_BLACK " 2. LD_LIBRARY_PATH was not set" CLEAR
                                      "\n"
                       : " 2. LD_LIBRARY_PATH was not set\n",
              stdout);
    } else {
        fputs(s->color ? BRIGHT_BLACK " 2. LD_LIBRARY_PATH:" CLEAR "\n"
                       : " 2. LD_LIBRARY_PATH:\n",
              stdout);
        print_colon_delimited_paths(
            s->string_table.arr + s->ld_library_path_offset, indent);
    }

    fputs(indent, stdout);
    if (runpath == NULL) {
        fputs(s->color ? BRIGHT_BLACK " 3. runpath was not set" CLEAR "\n"
                       : " 3. runpath was not set\n",
              stdout);
    } else {
        fputs(s->color ? BRIGHT_BLACK " 3. runpath:" CLEAR "\n"
                       : " 3. runpath:\n",
              stdout);
        print_colon_delimited_paths(runpath, indent);
    }

    fputs(indent, stdout);
    if (no_def_lib) {
        fputs(s->color ? BRIGHT_BLACK
                  " 4. ld.so.conf not considered due to NODEFLIB flag" CLEAR
                  "\n"
                       : " 4. ld.so.conf not considered due to NODEFLIB flag\n",
              stdout);
    } else {
        fputs(s->color ? BRIGHT_BLACK " 4. ld.so.conf:" CLEAR "\n"
                       : " 4. ld.so.conf:\n",
              stdout);
    }
    print_colon_delimited_paths(s->string_table.arr + s->ld_so_conf_offset,
                                indent);

    fputs(indent, stdout);
    if (no_def_lib) {
        fputs(s->color
                  ? BRIGHT_BLACK
                  " 5. Standard paths not considered due to NODEFLIB flag" CLEAR
                  "\n"
                  : " 4. Standard paths not considered due to NODEFLIB flag\n",
              stdout);
    } else {
        fputs(s->color ? BRIGHT_BLACK " 5. Standard paths:" CLEAR "\n"
                       : " 5. Standard paths:\n",
              stdout);
    }
    print_colon_delimited_paths(s->string_table.arr + s->default_paths_offset,
                                indent);

    free(indent);
}

static int visited_files_contains(struct visited_file_array_t *files,
                                  struct stat *needle) {
    for (size_t i = 0; i < files->n; ++i) {
        struct visited_file_t *f = &files->arr[i];
        if (f->st_dev == needle->st_dev && f->st_ino == needle->st_ino)
            return 1;
    }
    return 0;
}

static void visited_files_append(struct visited_file_array_t *files,
                                 struct stat *new) {
    if (files->n == files->capacity) {
        files->capacity *= 2;
        files->arr = realloc(files->arr, files->capacity);
        if (files->arr == NULL)
            exit(1);
    }
    files->arr[files->n].st_dev = new->st_dev;
    files->arr[files->n].st_ino = new->st_ino;
    ++files->n;
}

static int recurse(char *current_file, size_t depth, struct libtree_state_t *s,
                   elf_bits_t parent_bits, struct found_t reason) {
    FILE *fptr = fopen(current_file, "rb");
    if (fptr == NULL)
        return 1;

    // When we're done recursing, we should give back the memory we've claimed.
    size_t old_buf_size = s->string_table.n;

    // Parse the header
    char e_ident[16];
    if (fread(&e_ident, 16, 1, fptr) != 1) {
        fclose(fptr);
        return ERR_INVALID_MAGIC;
    }

    // Find magic elfs
    if (e_ident[0] != 0x7f || e_ident[1] != 'E' || e_ident[2] != 'L' ||
        e_ident[3] != 'F') {
        fclose(fptr);
        return ERR_INVALID_MAGIC;
    }

    // Do at least *some* header validation
    if (e_ident[4] != '\x01' && e_ident[4] != '\x02') {
        fclose(fptr);
        return ERR_INVALID_CLASS;
    }

    if (e_ident[5] != '\x01' && e_ident[5] != '\x02') {
        fclose(fptr);
        return ERR_INVALID_DATA;
    }

    elf_bits_t curr_bits = e_ident[4] == '\x02' ? BITS64 : BITS32;
    int is_little_endian = e_ident[5] == '\x01';

    // Make sure that we have matching bits with dependent
    if (parent_bits != EITHER && parent_bits != curr_bits) {
        fclose(fptr);
        return ERR_INVALID_BITS;
    }

    // Make sure that the elf file has a the host's endianness
    // Byte swapping is on the TODO list
    if (is_little_endian ^ host_is_little_endian()) {
        fclose(fptr);
        return ERR_UNSUPPORTED_ELF_FILE;
    }

    // And get the type
    union {
        struct header_64_t h64;
        struct header_32_t h32;
    } header;

    // Read the (rest of the) elf header
    if (curr_bits == BITS64) {
        if (fread(&header.h64, sizeof(struct header_64_t), 1, fptr) != 1) {
            fclose(fptr);
            return ERR_INVALID_HEADER;
        }
        if (header.h64.e_type != ET_EXEC && header.h64.e_type != ET_DYN) {
            fclose(fptr);
            return ERR_NO_EXEC_OR_DYN;
        }
        if (fseek(fptr, header.h64.e_phoff, SEEK_SET) != 0) {
            fclose(fptr);
            return ERR_INVALID_PHOFF;
        }
    } else {
        if (fread(&header.h32, sizeof(struct header_32_t), 1, fptr) != 1) {
            fclose(fptr);
            return ERR_INVALID_HEADER;
        }
        if (header.h32.e_type != ET_EXEC && header.h32.e_type != ET_DYN) {
            fclose(fptr);
            return ERR_NO_EXEC_OR_DYN;
        }
        if (fseek(fptr, header.h32.e_phoff, SEEK_SET) != 0) {
            fclose(fptr);
            return ERR_INVALID_PHOFF;
        }
    }

    // Make sure it's an executable or library
    union {
        struct prog_64_t p64;
        struct prog_32_t p32;
    } prog;

    // map vaddr to file offset (we don't mmap the file, but directly seek in
    // the file which means that we have to translate vaddr to file offset)
    struct small_vec_u64_t pt_load_offset;
    struct small_vec_u64_t pt_load_vaddr;

    small_vec_u64_init(&pt_load_offset);
    small_vec_u64_init(&pt_load_vaddr);

    // Read the program header.
    uint64_t p_offset = MAX_OFFSET_T;
    if (curr_bits == BITS64) {
        for (uint64_t i = 0; i < header.h64.e_phnum; ++i) {
            if (fread(&prog.p64, sizeof(struct prog_64_t), 1, fptr) != 1) {
                fclose(fptr);
                small_vec_u64_free(&pt_load_offset);
                small_vec_u64_free(&pt_load_vaddr);
                return ERR_INVALID_PROG_HEADER;
            }

            if (prog.p64.p_type == PT_LOAD) {
                small_vec_u64_append(&pt_load_offset, prog.p64.p_offset);
                small_vec_u64_append(&pt_load_vaddr, prog.p64.p_vaddr);
            } else if (prog.p64.p_type == PT_DYNAMIC) {
                p_offset = prog.p64.p_offset;
            }
        }
    } else {
        for (uint32_t i = 0; i < header.h32.e_phnum; ++i) {
            if (fread(&prog.p32, sizeof(struct prog_32_t), 1, fptr) != 1) {
                fclose(fptr);
                small_vec_u64_free(&pt_load_offset);
                small_vec_u64_free(&pt_load_vaddr);
                return ERR_INVALID_PROG_HEADER;
            }

            if (prog.p32.p_type == PT_LOAD) {
                small_vec_u64_append(&pt_load_offset, prog.p32.p_offset);
                small_vec_u64_append(&pt_load_vaddr, prog.p32.p_vaddr);
            } else if (prog.p32.p_type == PT_DYNAMIC) {
                p_offset = prog.p32.p_offset;
            }
        }
    }

    // At this point we're going to store the file as "success"
    struct stat finfo;
    if (stat(current_file, &finfo) != 0) {
        fclose(fptr);
        small_vec_u64_free(&pt_load_offset);
        small_vec_u64_free(&pt_load_vaddr);
        return ERR_CANT_STAT;
    }

    int seen_before = visited_files_contains(&s->visited, &finfo);

    if (!seen_before)
        visited_files_append(&s->visited, &finfo);

    // No dynamic section?
    if (p_offset == MAX_OFFSET_T) {
        print_line(depth, current_file, BOLD_CYAN, REGULAR_CYAN, 1, reason, s);
        fclose(fptr);
        small_vec_u64_free(&pt_load_offset);
        small_vec_u64_free(&pt_load_vaddr);
        return 0;
    }

    // I guess you always have to load at least a string
    // table, so if there are not PT_LOAD sections, then
    // it is an error.
    if (pt_load_offset.n == 0) {
        fclose(fptr);
        small_vec_u64_free(&pt_load_offset);
        small_vec_u64_free(&pt_load_vaddr);
        return ERR_NO_PT_LOAD;
    }

    // Go to the dynamic section
    if (fseek(fptr, p_offset, SEEK_SET) != 0) {
        fclose(fptr);
        small_vec_u64_free(&pt_load_offset);
        small_vec_u64_free(&pt_load_vaddr);
        return ERR_INVALID_DYNAMIC_SECTION;
    }

    // Shared libraries can disable searching in
    // "default" search paths, aka ld.so.conf and
    // /usr/lib etc. At least glibc respects this.
    int no_def_lib = 0;

    uint64_t strtab = MAX_OFFSET_T;
    uint64_t rpath = MAX_OFFSET_T;
    uint64_t runpath = MAX_OFFSET_T;
    uint64_t soname = MAX_OFFSET_T;

    // Offsets in strtab
    struct small_vec_u64_t needed;
    small_vec_u64_init(&needed);

    for (int cont = 1; cont;) {
        uint64_t d_tag;
        uint64_t d_val;

        if (curr_bits == BITS64) {
            struct dyn_64_t dyn;
            if (fread(&dyn, sizeof(struct dyn_64_t), 1, fptr) != 1) {
                fclose(fptr);
                small_vec_u64_free(&pt_load_offset);
                small_vec_u64_free(&pt_load_vaddr);
                small_vec_u64_free(&needed);
                return ERR_INVALID_DYNAMIC_ARRAY_ENTRY;
            }
            d_tag = dyn.d_tag;
            d_val = dyn.d_val;

        } else {
            struct dyn_32_t dyn;
            if (fread(&dyn, sizeof(struct dyn_32_t), 1, fptr) != 1) {
                fclose(fptr);
                small_vec_u64_free(&pt_load_offset);
                small_vec_u64_free(&pt_load_vaddr);
                small_vec_u64_free(&needed);
                return ERR_INVALID_DYNAMIC_ARRAY_ENTRY;
            }
            d_tag = dyn.d_tag;
            d_val = dyn.d_val;
        }

        // Store strtab / rpath / runpath / needed / soname info.
        switch (d_tag) {
        case DT_NULL:
            cont = 0;
            break;
        case DT_STRTAB:
            strtab = d_val;
            break;
        case DT_RPATH:
            rpath = d_val;
            break;
        case DT_RUNPATH:
            runpath = d_val;
            break;
        case DT_NEEDED:
            small_vec_u64_append(&needed, d_val);
            break;
        case DT_SONAME:
            soname = d_val;
            break;
        case DT_FLAGS_1:
            no_def_lib |= (DT_1_NODEFLIB & d_val) == DT_1_NODEFLIB;
            break;
        }
    }

    if (strtab == MAX_OFFSET_T) {
        fclose(fptr);
        small_vec_u64_free(&pt_load_offset);
        small_vec_u64_free(&pt_load_vaddr);
        small_vec_u64_free(&needed);
        return ERR_NO_STRTAB;
    }

    // Let's verify just to be sure that the offsets are
    // ordered.
    if (!is_ascending_order(pt_load_vaddr.p, pt_load_vaddr.n)) {
        fclose(fptr);
        small_vec_u64_free(&pt_load_vaddr);
        small_vec_u64_free(&pt_load_offset);
        small_vec_u64_free(&needed);
        return ERR_VADDRS_NOT_ORDERED;
    }

    // Find the file offset corresponding to the strtab virtual address
    size_t vaddr_idx = 0;
    while (vaddr_idx + 1 != pt_load_vaddr.n &&
           strtab >= pt_load_vaddr.p[vaddr_idx + 1]) {
        ++vaddr_idx;
    }

    uint64_t strtab_offset =
        pt_load_offset.p[vaddr_idx] + strtab - pt_load_vaddr.p[vaddr_idx];

    small_vec_u64_free(&pt_load_vaddr);
    small_vec_u64_free(&pt_load_offset);

    // From this point on we actually copy strings from the ELF file into our
    // own string buffer.

    // Copy the current soname
    size_t soname_buf_offset = s->string_table.n;
    if (soname != MAX_OFFSET_T) {
        if (fseek(fptr, strtab_offset + soname, SEEK_SET) != 0) {
            s->string_table.n = old_buf_size;
            fclose(fptr);
            small_vec_u64_free(&needed);
            return ERR_INVALID_SONAME;
        }
        string_table_copy_from_file(&s->string_table, fptr);
    }

    int in_exclude_list =
        soname != MAX_OFFSET_T &&
        is_in_exclude_list(s->string_table.arr + soname_buf_offset);

    // No need to recurse deeper when we aren't in very verbose mode.
    int should_recurse =
        depth < MAX_RECURSION_DEPTH &&
        ((!seen_before && !in_exclude_list) ||
         (!seen_before && in_exclude_list && s->verbosity >= 2) ||
         s->verbosity == 3);

    // Just print the library and return
    if (!should_recurse) {
        char *print_name = soname != MAX_OFFSET_T && !s->path
                               ? s->string_table.arr + soname_buf_offset
                               : current_file;
        char *bold_color = in_exclude_list ? REGULAR_MAGENTA : REGULAR_BLUE;
        char *regular_color = in_exclude_list ? REGULAR_MAGENTA : REGULAR_BLUE;
        print_line(depth, print_name, bold_color, regular_color, 0, reason, s);

        s->string_table.n = old_buf_size;
        fclose(fptr);
        small_vec_u64_free(&needed);
        return 0;
    }

    // Store the ORIGIN string.
    char origin[4096];
    char *last_slash = strrchr(current_file, '/');
    if (last_slash != NULL) {
        // we're also copying the last /.
        size_t bytes = last_slash - current_file + 1;
        memcpy(origin, current_file, bytes);
        origin[bytes + 1] = '\0';
    } else {
        // this only happens when the input is relative (e.g. in current dir)
        memcpy(origin, "./", 3);
    }

    // Copy DT_PRATH
    if (rpath == MAX_OFFSET_T) {
        s->rpath_offsets[depth] = SIZE_MAX;
    } else {
        s->rpath_offsets[depth] = s->string_table.n;
        if (fseek(fptr, strtab_offset + rpath, SEEK_SET) != 0) {
            s->string_table.n = old_buf_size;
            fclose(fptr);
            small_vec_u64_free(&needed);
            return ERR_INVALID_RPATH;
        }

        string_table_copy_from_file(&s->string_table, fptr);

        // We store the interpolated string right after the literal copy.
        size_t curr_buf_size = s->string_table.n;
        if (interpolate_variables(s, s->rpath_offsets[depth], origin))
            s->rpath_offsets[depth] = curr_buf_size;
    }

    // Copy DT_RUNPATH
    size_t runpath_buf_offset = s->string_table.n;
    if (runpath != MAX_OFFSET_T) {
        if (fseek(fptr, strtab_offset + runpath, SEEK_SET) != 0) {
            s->string_table.n = old_buf_size;
            fclose(fptr);
            small_vec_u64_free(&needed);
            return ERR_INVALID_RUNPATH;
        }

        string_table_copy_from_file(&s->string_table, fptr);

        // We store the interpolated string right after the literal copy.
        size_t curr_buf_size = s->string_table.n;
        if (interpolate_variables(s, runpath_buf_offset, origin))
            runpath_buf_offset = curr_buf_size;
    }

    // Copy needed libraries.
    struct small_vec_u64_t needed_buf_offsets;
    small_vec_u64_init(&needed_buf_offsets);

    for (size_t i = 0; i < needed.n; ++i) {
        small_vec_u64_append(&needed_buf_offsets, s->string_table.n);
        if (fseek(fptr, strtab_offset + needed.p[i], SEEK_SET) != 0) {
            s->string_table.n = old_buf_size;
            fclose(fptr);
            small_vec_u64_free(&needed_buf_offsets);
            small_vec_u64_free(&needed);
            return ERR_INVALID_NEEDED;
        }
        string_table_copy_from_file(&s->string_table, fptr);
    }

    fclose(fptr);

    char *print_name = soname == MAX_OFFSET_T || s->path
                           ? current_file
                           : (s->string_table.arr + soname_buf_offset);

    char *bold_color = in_exclude_list ? REGULAR_MAGENTA
                                       : seen_before ? REGULAR_BLUE : BOLD_CYAN;
    char *regular_color = in_exclude_list
                              ? REGULAR_MAGENTA
                              : seen_before ? REGULAR_BLUE : REGULAR_CYAN;

    int highlight = !seen_before && !in_exclude_list;
    print_line(depth, print_name, bold_color, regular_color, highlight, reason,
               s);

    // Finally start searching.

    size_t needed_not_found = needed_buf_offsets.n;

    // Skip common libraries if not verbose
    if (needed_not_found && s->verbosity == 0) {
        for (size_t i = 0; i < needed_not_found;) {
            // If in exclude list, swap to the back.
            if (is_in_exclude_list(s->string_table.arr +
                                   needed_buf_offsets.p[i])) {
                size_t tmp = needed_buf_offsets.p[i];
                needed_buf_offsets.p[i] =
                    needed_buf_offsets.p[needed_not_found - 1];
                needed_buf_offsets.p[--needed_not_found] = tmp;
                continue;
            } else {
                ++i;
            }
        }
    }

    // First go over absolute paths in needed libs.
    for (size_t i = 0; i < needed_not_found;) {
        char *name = s->string_table.arr + needed_buf_offsets.p[i];
        if (strchr(name, '/') != NULL) {
            // If it is not an absolute path, we bail, cause it then starts to
            // depend on the current working directory, which is rather
            // nonsensical. This is allowed by glibc though.
            s->found_all_needed[depth] = needed_not_found <= 1;
            if (name[0] != '/') {
                tree_preamble(s, depth + 1);
                if (s->color)
                    fputs(BOLD_RED, stdout);
                fputs(name, stdout);
                fputs(" is not absolute", stdout);
                fputs(s->color ? CLEAR "\n" : "\n", stdout);
            } else if (recurse(name, depth + 1, s, curr_bits,
                               (struct found_t){.how = DIRECT, .depth = 0}) !=
                       0) {
                tree_preamble(s, depth + 1);
                if (s->color)
                    fputs(BOLD_RED, stdout);
                fputs(name, stdout);
                fputs(" not found", stdout);
                fputs(s->color ? CLEAR "\n" : "\n", stdout);
            }

            // Even if not officially found, we mark it as found, cause we
            // handled the error here
            size_t tmp = needed_buf_offsets.p[i];
            needed_buf_offsets.p[i] =
                needed_buf_offsets.p[needed_not_found - 1];
            needed_buf_offsets.p[--needed_not_found] = tmp;
        } else {
            ++i;
        }
    }

    // Consider rpaths only when runpath is empty
    if (runpath == MAX_OFFSET_T) {
        // We have a stack of rpaths, try them all, starting with one set at
        // this lib, then the parents.
        for (int j = depth; j >= 0 && needed_not_found; --j) {
            if (s->rpath_offsets[j] == SIZE_MAX)
                continue;

            check_search_paths((struct found_t){.how = RPATH, .depth = j},
                               s->rpath_offsets[j], &needed_not_found,
                               &needed_buf_offsets, depth, s, curr_bits);
        }
    }

    // Then try LD_LIBRARY_PATH, if we have it.
    if (needed_not_found && s->ld_library_path_offset != SIZE_MAX) {
        check_search_paths((struct found_t){.how = LD_LIBRARY_PATH, .depth = 0},
                           s->ld_library_path_offset, &needed_not_found,
                           &needed_buf_offsets, depth, s, curr_bits);
    }

    // Then consider runpaths
    if (needed_not_found && runpath != MAX_OFFSET_T) {
        check_search_paths((struct found_t){.how = RUNPATH, .depth = 0},
                           runpath_buf_offset, &needed_not_found,
                           &needed_buf_offsets, depth, s, curr_bits);
    }

    // Check ld.so.conf paths
    if (!no_def_lib && needed_not_found) {
        check_search_paths((struct found_t){.how = LD_SO_CONF, .depth = 0},
                           s->ld_so_conf_offset, &needed_not_found,
                           &needed_buf_offsets, depth, s, curr_bits);
    }

    // Then consider standard paths
    if (!no_def_lib && needed_not_found) {
        check_search_paths((struct found_t){.how = DEFAULT, .depth = 0},
                           s->default_paths_offset, &needed_not_found,
                           &needed_buf_offsets, depth, s, curr_bits);
    }

    // Finally summarize those that could not be found.
    if (needed_not_found) {
        print_error(depth, needed_not_found, &needed_buf_offsets,
                    runpath == MAX_OFFSET_T
                        ? NULL
                        : s->string_table.arr + runpath_buf_offset,
                    s, no_def_lib);
        s->string_table.n = old_buf_size;
        small_vec_u64_free(&needed_buf_offsets);
        small_vec_u64_free(&needed);
        // return ERR_NOT_FOUND;
        return 0;
    }

    // Free memory in our string table
    s->string_table.n = old_buf_size;
    small_vec_u64_free(&needed_buf_offsets);
    small_vec_u64_free(&needed);
    return 0;
}

static int parse_ld_config_file(struct string_table_t *st, char *path);

static int ld_conf_globbing(struct string_table_t *st, char *pattern) {
    glob_t result;
    memset(&result, 0, sizeof(result));
    int status = glob(pattern, 0, NULL, &result);

    // Handle errors (no result is not an error...)
    switch (status) {
    case GLOB_NOSPACE:
    case GLOB_ABORTED:
        globfree(&result);
        return 1;
    case GLOB_NOMATCH:
        globfree(&result);
        return 0;
    }

    // Otherwise parse the files we've found!
    int code = 0;
    for (size_t i = 0; i < result.gl_pathc; ++i)
        code |= parse_ld_config_file(st, result.gl_pathv[i]);

    globfree(&result);
    return code;
}

static int parse_ld_config_file(struct string_table_t *st, char *path) {
    FILE *fptr = fopen(path, "r");

    if (fptr == NULL)
        return 1;

    char c = '0';
    char line[4096];

    while (c != EOF) {
        size_t line_len = 0;
        while ((c = getc(fptr)) != '\n' && c != EOF) {
            if (line_len < 4095) {
                line[line_len++] = c;
            }
        }

        line[line_len] = '\0';

        char *begin = line;
        char *end = line + line_len;
        // Remove leading whitespace
        for (; isspace(*begin); ++begin) {
        }

        // Remove trailing comments
        char *comment = strchr(begin, '#');
        if (comment != NULL)
            *comment = '\0';

        // Remove trailing whitespace
        // although, whitespace is technically allowed in paths :think:
        while (end != begin)
            if (!isspace(*--end))
                break;

        // Skip empty lines
        if (begin == end)
            continue;

        // Put back the end of the string
        end[1] = '\0';

        // 'include ': glob whatever follows.
        if (strncmp(begin, "include", 7) == 0 && isspace(begin[7])) {
            begin += 8;
            // Remove more whitespace.
            for (; isspace(*begin); ++begin) {
            }

            // String can't be empty as it was trimmed and
            // still had whitespace next to include.

            // TODO: check if relative globbing to the
            // current file is supported or not.
            // We do *not* support it right now.
            if (*begin != '/')
                continue;

            ld_conf_globbing(st, begin);
        } else {
            // Copy over and replace trailing \0 with :.
            string_table_store(st, begin);
            st->arr[st->n - 1] = ':';
        }
    }

    fclose(fptr);

    return 0;
}

static void parse_ld_so_conf(struct libtree_state_t *s) {
    struct string_table_t *st = &s->string_table;
    s->ld_so_conf_offset = st->n;

    // Linux / glibc
    parse_ld_config_file(st, "/etc/ld.so.conf");

    // FreeBSD
    parse_ld_config_file(st, "/etc/ld-elf.so.conf");

    // Replace the last semicolon with a '\0'
    // if we have a nonzero number of paths.
    if (st->n > s->ld_so_conf_offset) {
        st->arr[st->n - 1] = '\0';
    } else {
        string_table_store(st, "");
    }
}

static void parse_ld_library_path(struct libtree_state_t *s) {
    char *LD_LIBRARY_PATH = "LD_LIBRARY_PATH";
    s->ld_library_path_offset = SIZE_MAX;
    char *val = getenv(LD_LIBRARY_PATH);

    // not set, so nothing to do.
    if (val == NULL)
        return;

    s->ld_library_path_offset = s->string_table.n;

    // otherwise, we just copy it over and replace ; with :
    string_table_store(&s->string_table, val);

    // replace ; with :
    char *search = s->string_table.arr + s->ld_library_path_offset;
    while ((search = strchr(search, ';')) != NULL)
        *search++ = ':';
}

static void set_default_paths(struct libtree_state_t *s) {
    s->default_paths_offset = s->string_table.n;
    // TODO: how to retrieve this list properly at runtime?
    string_table_store(&s->string_table, "/lib:/lib64:/usr/lib:/usr/lib64");
}

static void libtree_state_init(struct libtree_state_t *s) {
    s->string_table.n = 0;
    s->string_table.capacity = 1024;
    s->string_table.arr = malloc(s->string_table.capacity);
    s->visited.n = 0;
    s->visited.capacity = 256;
    s->visited.arr = malloc(s->visited.capacity);
}

static void libtree_state_free(struct libtree_state_t *s) {
    free(s->string_table.arr);
    free(s->visited.arr);
}

static int print_tree(int pathc, char **pathv, struct libtree_state_t *s) {
    // First collect standard paths
    libtree_state_init(s);

    parse_ld_so_conf(s);
    parse_ld_library_path(s);
    set_default_paths(s);

    int libtree_last_err = 0;

    for (int i = 0; i < pathc; ++i) {
        int result = recurse(pathv[i], 0, s, EITHER,
                             (struct found_t){.how = INPUT, .depth = 0});
        if (result != 0)
            libtree_last_err = result;
    }

    libtree_state_free(s);
    return libtree_last_err;
}

int main(int argc, char **argv) {
    // Enable or disable colors (no-color.com)
    struct libtree_state_t s;
    s.color = getenv("NO_COLOR") == NULL && isatty(STDOUT_FILENO);
    s.verbosity = 0;
    s.path = 0;

    // We want to end up with an array of file names
    // in argv[1] up to argv[positional-1].
    int positional = 1;

    struct utsname uname_val;
    if (uname(&uname_val) != 0)
        return 1;

    // Technically this should be AT_PLATFORM, but
    // (a) the feature is rarely used
    // (b) it's almost always the same
    s.PLATFORM = uname_val.machine;
    s.OSNAME = uname_val.sysname;
    s.OSREL = uname_val.release;

    // TODO: how to find this value at runtime?
    s.LIB = "lib";

    int opt_help = 0;
    int opt_version = 0;

    // After `--` we treat everything as filenames, not flags.
    int opt_raw = 0;

    for (int i = 1; i < argc; ++i) {
        char *arg = argv[i];

        // Positional args don't start with - or are `-` literal.
        if (opt_raw || *arg != '-' || arg[1] == '\0') {
            argv[positional++] = arg;
            continue;
        }

        // Now we're in flag land!
        ++arg;

        // Long flags
        if (*arg == '-') {
            ++arg;

            // Literal '--'
            if (*arg == '\0') {
                opt_raw = 1;
                continue;
            }

            if (strcmp(arg, "version") == 0) {
                opt_version = 1;
            } else if (strcmp(arg, "path") == 0) {
                s.path = 1;
            } else if (strcmp(arg, "verbose") == 0) {
                ++s.verbosity;
            } else if (strcmp(arg, "help") == 0) {
                opt_help = 1;
            } else {
                fputs("Unrecognized flag `--", stderr);
                fputs(arg, stderr);
                fputs("`\n", stderr);
                return 1;
            }

            continue;
        }

        // Short flags
        for (; *arg != '\0'; ++arg) {
            switch (*arg) {
            case 'h':
                opt_help = 1;
                break;
            case 'p':
                s.path = 1;
                break;
            case 'v':
                ++s.verbosity;
                break;
            default:
                fputs("Unrecognized flag `-", stderr);
                fputs(arg, stderr);
                fputs("`\n", stderr);
                return 1;
            }
        }
    }

    ++argv;
    --positional;

    // Print a help message on -h, --help or no positional args.
    if (opt_help || (!opt_version && positional == 0)) {
        // clang-format off
        fputs("Show the dynamic dependency tree of ELF files\n"
              "Usage: libtree [OPTION]... [--] FILE [FILES]...\n"
              "\n"
              "  -h, --help     Print help info\n"
              "      --version  Print version info\n"
              "\n"
              "File names starting with '-', for example '-.so', can be specified as follows:\n"
              "  libtree -- -.so\n"
              "\n"
              "Locating libs options:\n"
              "  -p, --path     Show the path of libraries instead of the soname\n"
              "  -v             Show libraries skipped by default*\n"
              "  -vv            Show dependencies of libraries skipped by default*\n"
              "  -vvv           Show dependencies of already encountered libraries\n"
              "\n"
              "* For brevity, the following libraries are not shown by default:\n"
              "  ",
              stdout);
        // clang-format on

        // Print a comma separated list of skipped libraries,
        // with some new lines every now and then to make it readable.
        size_t num_excluded = sizeof(exclude_list) / sizeof(char *);

        size_t cursor_x = 3;
        for (size_t j = 0; j < num_excluded; ++j) {
            cursor_x += strlen(exclude_list[j]);
            if (cursor_x > 60) {
                cursor_x = 3;
                fputs("\n  ", stdout);
            }
            fputs(exclude_list[j], stdout);
            if (j + 1 != num_excluded)
                fputs(", ", stdout);
        }

        // rpath substitution values:
        fputs(".\n\nThe following rpath/runpath substitutions are used:\n", stdout);
        fputs("  PLATFORM       ", stdout);
        fputs(s.PLATFORM, stdout);
        fputs("\n  LIB            ", stdout);
        fputs(s.LIB, stdout);
        fputs("\n  OSNAME         ", stdout);
        fputs(s.OSNAME, stdout);
        fputs("\n  OSREL          ", stdout);
        fputs(s.OSREL, stdout);
        putchar('\n');

        // Return an error status code if no positional args were passed.
        return !opt_help;
    }

    if (opt_version) {
        puts(VERSION);
        return 0;
    }

    return print_tree(positional, argv, &s);
}
