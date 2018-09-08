#include <fcntl.h>
#include <iostream>
#include <linux/limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ProcIdlePages.h"

static unsigned long pagetype_size[16] = {
	// 4k page
	[PTE_HOLE] = PAGE_SIZE,
	[PTE_IDLE] = PAGE_SIZE,
	[PTE_ACCESSED] = PAGE_SIZE,

	// 2M page
	[PMD_HOLE] = PMD_SIZE,
	[PMD_IDLE] = PMD_SIZE,
	[PMD_ACCESSED] = PMD_SIZE,

	// 1G page
	[PUD_HOLE] = PUD_SIZE,
	[PUD_IDLE] = PUD_SIZE,
	[PUD_ACCESSED] = PUD_SIZE,

	// 512G
	[P4D_HOLE] = P4D_SIZE,
	[PGDIR_HOLE] = PGDIR_SIZE,
};

int ProcIdlePages::walk_multi(int nr, float interval)
{
  int err;

  auto maps = proc_maps.load(pid);
  if (maps.empty())
    return -ENOENT;

  nr_walks = nr; // for use by count_refs()
  page_refs_4k.clear();
  page_refs_2m.clear();
  page_refs_1g.clear();

  for (int i = 0; i < nr; ++i)
  {
    err = walk();
    if (err)
      return err;

    usleep(interval * 1000000);
  }

  return 0;
}

int ProcIdlePages::walk_vma(proc_maps_entry& vma)
{
    unsigned long va = vma.start;
    int rc = 0;

    proc_maps.show(vma);

    if (lseek(idle_fd, va_to_offset(va), SEEK_SET) == (off_t) -1)
    {
      printf(" error: seek for addr %lx failed, skip.\n", va);
      perror("lseek error");
      return -1;
    }

    for (; va < vma.end;)
    {
      off_t pos = lseek(idle_fd, 0, SEEK_CUR);
      if (pos == (off_t) -1) {
        perror("SEEK_CUR error");
        return -1;
      }
      if ((unsigned long)pos != va) {
        fprintf(stderr, "error pos != va: %lu %lu\n", pos, va);
        return -2;
      }

      rc = read(idle_fd, read_buf.data(), read_buf.size());
      if (rc < 0) {
        if (rc == -ENXIO || rc == -ERANGE)
          return 0;
        perror("read error");
        return rc;
      }

      if (!rc)
      {
        printf("read 0 size\n");
        return 0;
      }

      parse_idlepages(vma, va, rc);
    }

    return 0;
}

int ProcIdlePages::walk()
{
    std::vector<proc_maps_entry> address_map = proc_maps.load(pid);

    if (address_map.empty())
      return -ESRCH;

    int idle_fd = open_file();
    if (idle_fd < 0)
      return idle_fd;

    read_buf.resize(READ_BUF_SIZE);

    for (auto &vma: address_map)
      walk_vma(vma);

    close(idle_fd);

    return 0;
}

int ProcIdlePages::count_refs_one(
                   std::unordered_map<unsigned long, unsigned char>& page_refs,
                   std::vector<unsigned long>& refs_count)
{
    int err = 0;
    auto iter_beigin = page_refs.begin();
    auto iter_end = page_refs.end();

    refs_count.clear();

    refs_count.reserve(nr_walks + 1);

    for (size_t i = 0; i < refs_count.capacity(); ++i)
    {
        refs_count[i] = 0;
    }

    for(;iter_beigin != iter_end; ++iter_beigin)
    {
        refs_count[iter_beigin->second] += 1;
    }

    return err;
}

int ProcIdlePages::count_refs()
{
  int err = 0;

  err = count_refs_one(page_refs_4k, refs_count_4k);
  if (err) {
    std::cerr << "count 4K page out of range" << std::endl;
    return err;
  }

  err = count_refs_one(page_refs_2m, refs_count_2m);
  if (err) {
    std::cerr << "count 2M page out of range" << std::endl;
    return err;
  }

  return err;
}

int ProcIdlePages::save_counts(std::string filename)
{
  int err = 0;

  FILE *file;
  file = fopen(filename.c_str(), "w");
  if (!file) {
    std::cerr << "open file " << filename << "failed" << std::endl;
    perror(filename.c_str());
    return -1;
  }

  fprintf(file, "%-8s %-15s %-15s\n",
                "refs", "count_4K",
                "count_2M");
  fprintf(file, "=========================================================\n");

  for (int i = 0; i < nr_walks + 1; i++) {
    fprintf(file, "%-8d %-15lu %-15lu\n",
            i,
            refs_count_4k[i],
            refs_count_2m[i]);
  }
  fclose(file);

  return err;
}

const page_refs_map&
ProcIdlePages::get_page_refs(PageLevel level)
{
    switch(level)
    {
    case PageLevel::PAGE_4K:
        return page_refs_4k;
        break;
    case PageLevel::PAGE_2M:
        return page_refs_2m;
        break;
    case PageLevel::PAGE_1G:
        return page_refs_1g;
        break;

        //fall ok
    case PageLevel::BEGIN:
    case PageLevel::END:
    default:
        return page_refs_unknow;
        break;
    }
}


int ProcIdlePages::open_file()
{
    char filepath[PATH_MAX];

    memset(filepath, 0, sizeof(filepath));
    snprintf(filepath, sizeof(filepath), "/proc/%d/idle_bitmap", pid);

    idle_fd = open(filepath, O_RDWR);
    if (idle_fd < 0)
      perror(filepath);

    return idle_fd;
}

void ProcIdlePages::inc_page_refs(page_refs_map& page_refs,
                                  unsigned long va,
                                  unsigned long page_size,
                                  unsigned long count)
{
  for (unsigned long i = 0; i < count; ++i)
  {
    unsigned long vpfn = va / PAGE_SIZE;
    auto find_iter = page_refs.find(vpfn);

    if (find_iter == page_refs.end())
      page_refs[vpfn] = 1;
    else
      page_refs[vpfn] += 1;

    va += page_size;
  }
}

void ProcIdlePages::parse_idlepages(proc_maps_entry& vma,
                                    unsigned long& va,
                                    int bytes)
{
  for (int i = 0; i < bytes; ++i)
  {
    int nr = read_buf[i].nr;
    switch (read_buf[i].type)
    {
    case PTE_ACCESSED:
      inc_page_refs(page_refs_4k, va, PTE_SIZE, nr);
      break;
    case PMD_ACCESSED:
      inc_page_refs(page_refs_2m, va, PMD_SIZE, nr);
      break;
    case PUD_ACCESSED:
      inc_page_refs(page_refs_1g, va, PUD_SIZE, nr);
      break;
    }
    va += pagetype_size[read_buf[i].type] * nr;
  }
}

unsigned long ProcIdlePages::va_to_offset(unsigned long va)
{
  unsigned long offset = va;

  // offset /= PAGE_SIZE;
  offset &= ~(PAGE_SIZE - 1);

  return offset;
}


unsigned long ProcIdlePages::offset_to_va(unsigned long va)
{
  return va;
}
