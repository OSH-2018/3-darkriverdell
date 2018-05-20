#define FUSE_USE_VERSION 26
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fuse.h>
#include <sys/mman.h>
#define MAX_NAME 255
#define blocknr 16384U
#define blocksize 1024*256U
#define pagehead 40

#define min(a,b) (((a)<(b))?(a):(b))

#define OFFSET 0
#define NEXT 1

typedef struct file{
	char *filename;
	void *content;
	struct stat *st;
	int head_size;
	int head_page;
	struct file *next;
} node;

static void *mem[blocknr]; //all pointers guaranteed to be NULL
typedef int pagenum;

pagenum page_avalible() {
	int last_init_page = *(int*)mem[0];
	for (int i = (last_init_page + 1) % blocknr; i != last_init_page; i = (i + 1) % blocknr) {
		if (!mem[i]) {
			*(int*)mem[0] = i;
			return i;
		}
	}
	return -1;
}

void set_page_next(pagenum current, pagenum next)
{
	*((int*)(mem[current]) + NEXT) = next;
	return;
}

void set_page_offset(pagenum current, int offset)
{
	*((int*)(mem[current]) + OFFSET) = offset;
	return;
}

int page_offset(pagenum current) {
	if (!mem[current]) return -1;
	return *((int*)mem[current] + OFFSET);
}

pagenum page_next(pagenum current) {
	if (!mem[current]) {
		return -1;
	}
	return *((int*)mem[current] + NEXT);
}

static int init_page(pagenum current) {

	if (mem[current]) {
		return -1;
	}//has already been used

	mem[current] = mmap(NULL, blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	memset(mem[current], 0, blocksize);
	set_page_offset(current, pagehead);
	return 0;
}

static int init_next_page(pagenum current_page) {
	int next = page_avalible();
	init_page(next);
	set_page_next(current_page, next);
	set_page_offset(current_page, blocksize);
	return next;
}

static void free_all_linked_pages(pagenum current)
{
	if (current == 0)
		return;
	free_all_linked_pages(page_next(current));
	munmap(mem[current], blocksize);
	return;
}

static void *get_mem(pagenum current, int size) {
	if (!mem[current]) {
		return NULL;
	}
	int offset = page_offset(current);
	set_page_offset(current, offset + size);
	return (void*)((char*)mem[current] + offset);
}

void set_root(node *root) 
{
	node **rootptr = (node**)(mem[0] + pagehead);
	*rootptr = root;
	return;
}

node* get_root() 
{
	return *(node**)(mem[0] + pagehead);
}

static node *get_filenode(const char *name) {
	node *node = get_root();
	while (node) {
		if (strcmp(node->filename, name + 1) != 0)
			node = node->next;
		else
			return node;
	}
	return NULL;
}

static int create_filenode(const char *filename, const struct stat *st) {
	//init a page
	int i = page_avalible();
	init_page(i);

	//set file infomation
	node *new = (node *)get_mem(i, sizeof(node));
	if (strlen(filename) < MAX_NAME)
	{
		new->filename = (char *)get_mem(i, strlen(filename) + 1);
	}
	else
		return -1;
	memcpy(new->filename, filename, strlen(filename) + 1);
	new->st = (struct stat *)get_mem(i, sizeof(struct stat));
	memcpy(new->st, st, sizeof(struct stat));
	new->head_size = page_offset(i);
	new->content = mem[i] + page_offset(i);
	new->head_page = i;

	//insert to the file-link
	node *root = get_root();
	new->next = root;
	set_root(new);
	return 0;
}

static void *oshfs_init(struct fuse_conn_info *conn) {
	init_page(0);
	node **rootptr = (node**)get_mem(0, sizeof(node*));
	*rootptr = NULL;
	*(int*)mem[0] = 0; //last_init_page
	return NULL;
}

static int oshfs_getattr(const char *path, struct stat *stbuf) {
	int ret = 0;
	node *node = get_filenode(path);
	if (strcmp(path, "/") == 0) {
		memset(stbuf, 0, sizeof(struct stat));
		stbuf->st_mode = S_IFDIR | 0755;
	}
	else if (node) {
		memcpy(stbuf, node->st, sizeof(struct stat));
	}
	else {
		ret = -ENOENT;
	}
	return ret;
}

static int oshfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
	node *node = get_root();
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	while (node) {
		filler(buf, node->filename, node->st, 0);
		node = node->next;
	}

	return 0;
}

static int oshfs_mknod(const char *path, mode_t mode, dev_t dev) {
	struct stat st;
	st.st_mode = S_IFREG | 0644;
	st.st_uid = fuse_get_context()->uid;
	st.st_gid = fuse_get_context()->gid;
	st.st_nlink = 1;
	st.st_size = 0;
	create_filenode(path + 1, &st);
	return 0;
}

static int oshfs_open(const char *path, struct fuse_file_info *fi) {
	return 0;
}

static int oshfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	node *node = get_filenode(path);
	if (offset + size > node->st->st_size)
		node->st->st_size = offset + size;
	int full_page_size = blocksize - pagehead;
	pagenum current = node->head_page;
	int used_size = blocksize - node->head_size;
	while (used_size < offset)
	{
		if (page_next(current) == 0)
		{
			init_next_page(current);
		}
		current = page_next(current);
		used_size += full_page_size;
	}
	int written_size = 0;
	memcpy(mem[current] + blocksize - (used_size - offset), buf, min(size, used_size - offset));
	free_all_linked_pages(page_next(current));
	written_size += used_size - offset;
	while (written_size < size) 
	{
		if (page_next(current) == 0) 
		{
			init_next_page(current);
		}
		current = page_next(current);
		memcpy(mem[current] + pagehead, buf + written_size, min(size - written_size, full_page_size));
		written_size += full_page_size;
	}
	set_page_offset(current, blocksize - written_size + size);
	return size;
}

static int oshfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	node *node = get_filenode(path);
	pagenum current_page = node->head_page;
	int used_size = blocksize - node->head_size;
	int full_page_size = blocksize - pagehead;
	if (offset + size > node->st->st_size)
		size = node->st->st_size-offset;

	while (used_size < offset)
	{
		current_page = page_next(current_page);
		used_size += full_page_size;
	}
	memcpy(buf, mem[current_page] + blocksize - used_size + offset, min(size, used_size - offset));
	int read_size = used_size - offset;
	while (read_size < size) {
		current_page = page_next(current_page);
		if (current_page == 0) return -1;
		memcpy(buf + read_size, mem[current_page] + pagehead, min(size - read_size, full_page_size));
		read_size += full_page_size;
	}
	return size;
}

static int oshfs_truncate(const char *path, off_t size) {
	node *node = get_filenode(path);
	node->st->st_size = size;
	int current_page = node->head_page;
	int used_size = blocksize - node->head_size;
	while (used_size < size) 
	{
		if (page_next(current_page) == 0) init_next_page(current_page);
		current_page = page_next(current_page);
		used_size += blocksize - pagehead;
	}
	set_page_offset(current_page, blocksize - (used_size - size));
	free_all_linked_pages(page_next(current_page));
	set_page_next(current_page, 0);
	return 0;
}

static int oshfs_unlink(const char *path) {
	node *root = get_root();
	node *unlink_node = get_filenode(path);
	int current_page = unlink_node->head_page;
	if (unlink_node == root)
	{
		set_root(unlink_node->next);
	}
	else
	{
		node* i;
		for (i = root; i->next != unlink_node; i = i->next)
			;
		i->next = unlink_node->next;
	}
	free_all_linked_pages(current_page);
	return 0;
}

static const struct fuse_operations op = {
	.init = oshfs_init,
	.getattr = oshfs_getattr,
	.readdir = oshfs_readdir,
	.mknod = oshfs_mknod,
	.open = oshfs_open,
	.write = oshfs_write,
	.truncate = oshfs_truncate,
	.read = oshfs_read,
	.unlink = oshfs_unlink,
};

int main(int argc, char *argv[]) {
	return fuse_main(argc, argv, &op, NULL);
}
