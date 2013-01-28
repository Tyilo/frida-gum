/*
 * Copyright (C) 2010-2012 Ole André Vadla Ravnås <ole.andre.ravnas@tillitech.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "gumprocess.h"

#include "gumlinux.h"

#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>

#define GUM_HIJACK_SIGNAL (SIGRTMIN + 7)

#if GLIB_SIZEOF_VOID_P == 4
typedef Elf32_Ehdr GumElfEHeader;
typedef Elf32_Shdr GumElfSHeader;
typedef Elf32_Sym GumElfSymbol;
# define GUM_ELF_ST_BIND(val) ELF32_ST_BIND(val)
# define GUM_ELF_ST_TYPE(val) ELF32_ST_TYPE(val)
#else
typedef Elf64_Ehdr GumElfEHeader;
typedef Elf64_Shdr GumElfSHeader;
typedef Elf64_Sym GumElfSymbol;
# define GUM_ELF_ST_BIND(val) ELF64_ST_BIND(val)
# define GUM_ELF_ST_TYPE(val) ELF64_ST_TYPE(val)
#endif

#define GUM_MAPS_LINE_SIZE (1024 + PATH_MAX)

typedef struct _GumFindModuleContext GumFindModuleContext;
typedef struct _GumFindExportContext GumFindExportContext;

struct _GumFindModuleContext
{
  const gchar * module_name;
  GumAddress base;
  gchar * path;
};

struct _GumFindExportContext
{
  GumAddress result;
  const gchar * symbol_name;
};

static void gum_do_modify_thread (int sig, siginfo_t * siginfo,
    void * context);
static void gum_store_cpu_context (GumThreadId thread_id,
    GumCpuContext * cpu_context, gpointer user_data);

static gboolean gum_store_base_and_path_if_name_matches (const gchar * name,
    GumAddress address, const gchar * path, gpointer user_data);
static gboolean gum_store_address_if_export_name_matches (const gchar * name,
    GumAddress address, gpointer user_data);

static void gum_cpu_context_from_linux (const ucontext_t * uc,
    GumCpuContext * ctx);
static void gum_cpu_context_to_linux (const GumCpuContext * ctx,
    ucontext_t * uc);
static GumThreadState gum_thread_state_from_proc_status_character (gchar c);
static GumPageProtection gum_page_protection_from_proc_perms_string (
    const gchar * perms);

G_LOCK_DEFINE_STATIC (gum_modify_thread);
static volatile gboolean gum_modify_thread_did_load_cpu_context;
static volatile gboolean gum_modify_thread_did_modify_cpu_context;
static volatile gboolean gum_modify_thread_did_store_cpu_context;
static GumCpuContext gum_modify_thread_cpu_context;

GumThreadId
gum_process_get_current_thread_id (void)
{
  return syscall (SYS_gettid);
}

gboolean
gum_process_modify_thread (GumThreadId thread_id,
                           GumModifyThreadFunc func,
                           gpointer user_data)
{
  gboolean success = FALSE;
  struct sigaction action, old_action;

  if (thread_id == gum_process_get_current_thread_id ())
  {
    ucontext_t uc;
    volatile gboolean modified = FALSE;

    getcontext (&uc);
    if (!modified)
    {
      GumCpuContext cpu_context;

      gum_cpu_context_from_linux (&uc, &cpu_context);
      func (thread_id, &cpu_context, user_data);
      gum_cpu_context_to_linux (&cpu_context, &uc);

      modified = TRUE;
      setcontext (&uc);
    }

    success = TRUE;
  }
  else
  {
    G_LOCK (gum_modify_thread);

    gum_modify_thread_did_load_cpu_context = FALSE;
    gum_modify_thread_did_modify_cpu_context = FALSE;
    gum_modify_thread_did_store_cpu_context = FALSE;

    action.sa_sigaction = gum_do_modify_thread;
    sigemptyset (&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    sigaction (GUM_HIJACK_SIGNAL, &action, &old_action);

    if (syscall (SYS_tgkill, getpid (), thread_id, GUM_HIJACK_SIGNAL) == 0)
    {
      /* FIXME: timeout? */
      while (!gum_modify_thread_did_load_cpu_context)
        g_thread_yield ();
      func (thread_id, &gum_modify_thread_cpu_context, user_data);
      gum_modify_thread_did_modify_cpu_context = TRUE;
      while (!gum_modify_thread_did_store_cpu_context)
        g_thread_yield ();

      success = TRUE;
    }

    sigaction (GUM_HIJACK_SIGNAL, &old_action, NULL);

    G_UNLOCK (gum_modify_thread);
  }

  return success;
}

static void
gum_do_modify_thread (int sig,
                      siginfo_t * siginfo,
                      void * context)
{
  ucontext_t * uc = (ucontext_t *) context;

  gum_cpu_context_from_linux (uc, &gum_modify_thread_cpu_context);
  gum_modify_thread_did_load_cpu_context = TRUE;
  while (!gum_modify_thread_did_modify_cpu_context)
    ;
  gum_cpu_context_to_linux (&gum_modify_thread_cpu_context, uc);
  gum_modify_thread_did_store_cpu_context = TRUE;
}

void
gum_process_enumerate_threads (GumFoundThreadFunc func,
                               gpointer user_data)
{
  GDir * dir;
  const gchar * name;
  gboolean carry_on = TRUE;

  dir = g_dir_open ("/proc/self/task", 0, NULL);
  g_assert (dir != NULL);

  while (carry_on && (name = g_dir_read_name (dir)) != NULL)
  {
    gchar * path, * info = NULL;

    path = g_strconcat ("/proc/self/task/", name, "/stat", NULL);
    if (g_file_get_contents (path, &info, NULL, NULL))
    {
      gchar * state;
      GumThreadDetails details;

      state = strrchr (info, ')') + 2;

      details.id = atoi (name);
      details.state = gum_thread_state_from_proc_status_character (*state);
      if (gum_process_modify_thread (details.id, gum_store_cpu_context,
            &details.cpu_context))
      {
        carry_on = func (&details, user_data);
      }
    }

    g_free (info);
    g_free (path);
  }

  g_dir_close (dir);
}

static void
gum_store_cpu_context (GumThreadId thread_id,
                       GumCpuContext * cpu_context,
                       gpointer user_data)
{
  memcpy (user_data, cpu_context, sizeof (GumCpuContext));
}

void
gum_process_enumerate_modules (GumFoundModuleFunc func,
                               gpointer user_data)
{
  FILE * fp;
  const guint line_size = GUM_MAPS_LINE_SIZE;
  gchar * line, * path, * prev_path;
  gboolean carry_on = TRUE;

  fp = fopen ("/proc/self/maps", "r");
  g_assert (fp != NULL);

  line = g_malloc (line_size);

  path = g_malloc (PATH_MAX);
  prev_path = g_malloc (PATH_MAX);
  prev_path[0] = '\0';

  while (carry_on && fgets (line, line_size, fp) != NULL)
  {
    const guint8 elf_magic[] = { 0x7f, 'E', 'L', 'F' };
    guint8 * start;
    gint n;
    gchar * name;

    n = sscanf (line, "%p-%*p %*s %*x %*s %*s %s", &start, path);
    if (n == 1)
      continue;
    g_assert_cmpint (n, ==, 2);

    if (strcmp (path, prev_path) == 0 || path[0] == '[')
      continue;
    else if (memcmp (start, elf_magic, sizeof (elf_magic)) != 0)
      continue;

    name = g_path_get_basename (path);
    carry_on = func (name, GUM_ADDRESS (start), path, user_data);
    g_free (name);

    strcpy (prev_path, path);
  }

  g_free (path);
  g_free (prev_path);

  g_free (line);

  fclose (fp);
}

void
gum_process_enumerate_ranges (GumPageProtection prot,
                              GumFoundRangeFunc func,
                              gpointer user_data)
{
  gum_linux_enumerate_ranges (getpid (), prot, func, user_data);
}

void
gum_linux_enumerate_ranges (pid_t pid,
                            GumPageProtection prot,
                            GumFoundRangeFunc func,
                            gpointer user_data)
{
  gchar * maps_path;
  FILE * fp;
  const guint line_size = GUM_MAPS_LINE_SIZE;
  gchar * line;
  gboolean carry_on = TRUE;

  maps_path = g_strdup_printf ("/proc/%d/maps", pid);

  fp = fopen (maps_path, "r");
  g_assert (fp != NULL);

  g_free (maps_path);

  line = g_malloc (line_size);

  while (carry_on && fgets (line, line_size, fp) != NULL)
  {
    GumAddress start, end;
    gchar perms[4 + 1] = { 0, };
    gint n;
    GumPageProtection cur_prot;

    n = sscanf (line, "%" G_GINT64_MODIFIER "x-%" G_GINT64_MODIFIER "x %4s", &start, &end, perms);
    g_assert_cmpint (n, ==, 3);

    cur_prot = gum_page_protection_from_proc_perms_string (perms);

    if ((cur_prot & prot) == prot)
    {
      GumMemoryRange range;

      range.base_address = GUM_ADDRESS (start);
      range.size = end - start;

      carry_on = func (&range, cur_prot, user_data);
    }
  }

  g_free (line);

  fclose (fp);
}

void
gum_module_enumerate_exports (const gchar * module_name,
                              GumFoundExportFunc func,
                              gpointer user_data)
{
  GumFindModuleContext ctx = { module_name, 0, NULL };
  gint fd = -1;
  gsize file_size;
  gpointer base_address = NULL;
  GumElfEHeader * ehdr;
  guint i;
  gsize dynsym_section_offset = 0, dynsym_section_size = 0;
  gsize dynsym_entry_size = 0;
  const gchar * dynsym_strtab = NULL;

  gum_process_enumerate_modules (gum_store_base_and_path_if_name_matches,
      &ctx);
  if (ctx.base == 0)
    goto beach;

  fd = open (ctx.path, O_RDONLY);
  if (fd == -1)
    goto beach;

  file_size = lseek (fd, 0, SEEK_END);
  lseek (fd, 0, SEEK_SET);

  base_address = mmap (NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
  g_assert (base_address != MAP_FAILED);

  ehdr = base_address;
  if (ehdr->e_type != ET_DYN)
    goto beach;

  for (i = 0; i != ehdr->e_shnum; i++)
  {
    GumElfSHeader * shdr;

    shdr = base_address + ehdr->e_shoff + (i * ehdr->e_shentsize);
    if (shdr->sh_type == SHT_DYNSYM)
    {
      GumElfSHeader * strtab_shdr;

      dynsym_section_offset = shdr->sh_offset;
      dynsym_section_size = shdr->sh_size;
      dynsym_entry_size = shdr->sh_entsize;

      strtab_shdr = base_address + ehdr->e_shoff +
          (shdr->sh_link * ehdr->e_shentsize);
      dynsym_strtab = base_address + strtab_shdr->sh_offset;

      g_assert_cmpuint (dynsym_section_size % dynsym_entry_size, ==, 0);
    }
  }

  if (dynsym_section_offset == 0)
    goto beach;

  for (i = 0; i != dynsym_section_size / dynsym_entry_size; i++)
  {
    GumElfSymbol * sym;

    sym = base_address + dynsym_section_offset + (i * dynsym_entry_size);
    if ((GUM_ELF_ST_BIND (sym->st_info) == STB_GLOBAL ||
         GUM_ELF_ST_BIND (sym->st_info) == STB_WEAK) &&
        GUM_ELF_ST_TYPE (sym->st_info) == STT_FUNC &&
        sym->st_shndx != SHN_UNDEF)
    {
      const gchar * name;
      GumAddress address;

      name = dynsym_strtab + sym->st_name;
      address = ctx.base + sym->st_value;

      if (!func (name, address, user_data))
        goto beach;
    }
  }

beach:
  if (base_address != NULL)
    munmap (base_address, file_size);

  if (fd != -1)
    close (fd);

  g_free (ctx.path);
}

void
gum_module_enumerate_ranges (const gchar * module_name,
                             GumPageProtection prot,
                             GumFoundRangeFunc func,
                             gpointer user_data)
{
  FILE * fp;
  const guint line_size = GUM_MAPS_LINE_SIZE;
  gchar * line, * path;
  gboolean carry_on = TRUE;

  fp = fopen ("/proc/self/maps", "r");
  g_assert (fp != NULL);

  line = g_malloc (line_size);
  path = g_malloc (PATH_MAX);

  while (carry_on && fgets (line, line_size, fp) != NULL)
  {
    guint8 * start, * end;
    gchar perms[4 + 1] = { 0, };
    gint n;
    gchar * name;

    n = sscanf (line, "%p-%p %4s %*x %*s %*s %s", &start, &end, perms, path);
    if (n == 3)
      continue;
    g_assert_cmpint (n, ==, 4);

    if (path[0] == '[')
      continue;

    name = g_path_get_basename (path);
    if (strcmp (name, module_name) == 0)
    {
      GumPageProtection cur_prot;

      cur_prot = gum_page_protection_from_proc_perms_string (perms);

      if ((cur_prot & prot) == prot)
      {
        GumMemoryRange range;

        range.base_address = GUM_ADDRESS (start);
        range.size = end - start;

        carry_on = func (&range, cur_prot, user_data);
      }
    }
    g_free (name);
  }

  g_free (path);
  g_free (line);

  fclose (fp);
}

GumAddress
gum_module_find_base_address (const gchar * module_name)
{
  GumFindModuleContext ctx = { module_name, 0, NULL };
  gum_process_enumerate_modules (gum_store_base_and_path_if_name_matches, &ctx);
  g_free (ctx.path);
  return ctx.base;
}

GumAddress
gum_module_find_export_by_name (const gchar * module_name,
                                const gchar * symbol_name)
{
  GumFindExportContext ctx;

  ctx.result = 0;
  ctx.symbol_name = symbol_name;

  gum_module_enumerate_exports (module_name,
      gum_store_address_if_export_name_matches, &ctx);

  return ctx.result;
}

static gboolean
gum_store_base_and_path_if_name_matches (const gchar * name,
                                         GumAddress address,
                                         const gchar * path,
                                         gpointer user_data)
{
  GumFindModuleContext * ctx = user_data;

  if (strcmp (name, ctx->module_name) != 0)
    return TRUE;

  ctx->base = address;
  ctx->path = g_strdup (path);
  return FALSE;
}

static gboolean
gum_store_address_if_export_name_matches (const gchar * name,
                                          GumAddress address,
                                          gpointer user_data)
{
  GumFindExportContext * ctx = (GumFindExportContext *) user_data;

  if (strcmp (name, ctx->symbol_name) == 0)
  {
    ctx->result = address;
    return FALSE;
  }

  return TRUE;
}

static void
gum_cpu_context_from_linux (const ucontext_t * uc,
                            GumCpuContext * ctx)
{
#if defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 4
  const greg_t * gr = uc->uc_mcontext.gregs;

  ctx->eip = gr[REG_EIP];

  ctx->edi = gr[REG_EDI];
  ctx->esi = gr[REG_ESI];
  ctx->ebp = gr[REG_EBP];
  ctx->esp = gr[REG_ESP];
  ctx->ebx = gr[REG_EBX];
  ctx->edx = gr[REG_EDX];
  ctx->ecx = gr[REG_ECX];
  ctx->eax = gr[REG_EAX];
#elif defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 8
  const greg_t * gr = uc->uc_mcontext.gregs;

  ctx->rip = gr[REG_RIP];

  ctx->r15 = gr[REG_R15];
  ctx->r14 = gr[REG_R14];
  ctx->r13 = gr[REG_R13];
  ctx->r12 = gr[REG_R12];
  ctx->r11 = gr[REG_R11];
  ctx->r10 = gr[REG_R10];
  ctx->r9 = gr[REG_R9];
  ctx->r8 = gr[REG_R8];

  ctx->rdi = gr[REG_RDI];
  ctx->rsi = gr[REG_RSI];
  ctx->rbp = gr[REG_RBP];
  ctx->rsp = gr[REG_RSP];
  ctx->rbx = gr[REG_RBX];
  ctx->rdx = gr[REG_RDX];
  ctx->rcx = gr[REG_RCX];
  ctx->rax = gr[REG_RAX];
#else
# error FIXME
#endif
}

static void
gum_cpu_context_to_linux (const GumCpuContext * ctx,
                          ucontext_t * uc)
{
#if defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 4
  greg_t * gr = uc->uc_mcontext.gregs;

  gr[REG_EIP] = ctx->eip;

  gr[REG_EDI] = ctx->edi;
  gr[REG_ESI] = ctx->esi;
  gr[REG_EBP] = ctx->ebp;
  gr[REG_ESP] = ctx->esp;
  gr[REG_EBX] = ctx->ebx;
  gr[REG_EDX] = ctx->edx;
  gr[REG_ECX] = ctx->ecx;
  gr[REG_EAX] = ctx->eax;
#elif defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 8
  greg_t * gr = uc->uc_mcontext.gregs;

  gr[REG_RIP] = ctx->rip;

  gr[REG_R15] = ctx->r15;
  gr[REG_R14] = ctx->r14;
  gr[REG_R13] = ctx->r13;
  gr[REG_R12] = ctx->r12;
  gr[REG_R11] = ctx->r11;
  gr[REG_R10] = ctx->r10;
  gr[REG_R9] = ctx->r9;
  gr[REG_R8] = ctx->r8;

  gr[REG_RDI] = ctx->rdi;
  gr[REG_RSI] = ctx->rsi;
  gr[REG_RBP] = ctx->rbp;
  gr[REG_RSP] = ctx->rsp;
  gr[REG_RBX] = ctx->rbx;
  gr[REG_RDX] = ctx->rdx;
  gr[REG_RCX] = ctx->rcx;
  gr[REG_RAX] = ctx->rax;
#else
# error FIXME
#endif
}

static GumThreadState
gum_thread_state_from_proc_status_character (gchar c)
{
  switch (c)
  {
    case 'R': return GUM_THREAD_RUNNING;
    case 'S': return GUM_THREAD_WAITING;
    case 'D': return GUM_THREAD_UNINTERRUPTIBLE;
    case 'Z': return GUM_THREAD_UNINTERRUPTIBLE;
    case 'T': return GUM_THREAD_STOPPED;
    case 'W': return GUM_THREAD_UNINTERRUPTIBLE;
    default:
      g_assert_not_reached ();
      break;
  }
}

static GumPageProtection
gum_page_protection_from_proc_perms_string (const gchar * perms)
{
  GumPageProtection prot = GUM_PAGE_NO_ACCESS;

  if (perms[0] == 'r')
    prot |= GUM_PAGE_READ;
  if (perms[1] == 'w')
    prot |= GUM_PAGE_WRITE;
  if (perms[2] == 'x')
    prot |= GUM_PAGE_EXECUTE;

  return prot;
}