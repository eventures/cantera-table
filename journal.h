#ifndef JOURNAL_H_
#define JOURNAL_H_ 1

enum journal_operation
{
  JOURNAL_TRUNCATE = 1,
  JOURNAL_CREATE_FILE = 2
};

void
journal_init (const char *path);

int
journal_file_open (const char *name);

off_t
journal_file_size (int handle);

void
journal_file_append (int handle, const void *data, size_t size);

void
journal_flush (void);

void
journal_commit (void);

#endif /* !JOURNAL_H_ */
