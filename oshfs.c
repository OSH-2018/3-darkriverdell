#define FUSE_USE_VERSION 26

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fuse.h>
#include <sys/mman.h>
#include <stdio.h>


#define BITMAP_BEGIN 1
#define BITMAP_SIZE 256
#define INODE_BITMAP_SIZE 32
#define INODE_BEGIN 257
#define INODE_SIZE 32*1024U
#define DATA_BEGIN 295936U

#define FREE 0
#define INUSE 1

#define REGULAR 0
#define DIRECTORY 1

#define ROOTNUMBER 0 //root inumber is 0
#define LONGESTPATH 255

static const size_t size = 4 * 1024 * 1024 * (size_t)1024; //4GB
static const size_t blocksize = 4 * (size_t)1024; //4KB blocksize
static const size_t sectorsize = 512; //512B per inode

typedef union inode{
	unsigned char filetype_reserve; // regular or directory
	struct stat* st; // status
	struct stat status;
	int inum;
	int head_page_number;
	union inode* next; // next avalible inode
	char *filename; //255B maximum

	unsigned char padding[512];
} inode;

typedef struct filepage{
	int page_number;
	void* content;
	struct filepage* nextpage;
}filepage;

typedef struct {
	filepage* page;
	char* page_offset;
}seek_tuple;

static void *mem[1024*1024]; //1024*1024 blocks
//static const inode *rootptr; //pointer to root inode
//static const unsigned char* i_bitmap;
//static const unsigned char* d_bitmap;

inode *get_filenode(const char *name)
{
	inode *node = (inode*) mem[INODE_BEGIN];
	while (node) {
		if (strcmp(node->filename, name + 1) != 0)
			node = node->next;
		else
			return node;
	}
	return NULL;
}

static inode* creat_inode_sector()
{
	int i, j;
	int avalible = 0;
	char *bitmap;
	for (i = 0; i < INODE_BITMAP_SIZE; i++) {
		bitmap = (char*)mem[BITMAP_BEGIN + i];
		for (j = 0; j < blocksize; j++) {
			if (bitmap[j] == 0) {
				avalible = 1;
				break;
			}
		}
	}
	if (avalible == 0) {
		return NULL;
	}
	if (i*blocksize + j >= INODE_SIZE * 8) {
		return NULL;
	}
	int inode_block = (i*blocksize + j) / 8;
	int inode_sector = (i*blocksize + j) % 8;
	if ((j % 8) == 0){
		bitmap[j] = INUSE;
	}
	else {
		mem[INODE_BEGIN + inode_block] = mmap(NULL, blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		bitmap[j] = INUSE;
	}
	inode *new_inode = (inode*)(mem[INODE_BEGIN + inode_block]) + inode_sector;
	new_inode->inum = i*blocksize + j;
	return new_inode;
}

static filepage* creat_filepage(int *head_page_number)
{
	char* bitmap;
	int avalible = 0;
	int i, j;
	for (i = 0; i < BITMAP_SIZE - INODE_BITMAP_SIZE; i++)
	{
		bitmap = mem[BITMAP_BEGIN + INODE_BITMAP_SIZE + i];
		for (j = 0; j < blocksize ; j++)
		{
			if (bitmap[j] == 0) {
				avalible = 1;
				break;
			}
		}
	}
	if (avalible == 0) {
		return NULL;
	}
	if (i*blocksize + j >= (size / blocksize) - DATA_BEGIN) {
		return NULL;
	}
	bitmap[j] = INUSE;
	mem[i*blocksize + j + DATA_BEGIN] = mmap(NULL, blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (head_page_number) {
		*head_page_number = i*blocksize + j + DATA_BEGIN;
	}
	filepage* new_page = (filepage*)mem[i*blocksize + j + DATA_BEGIN];
	new_page->content = (char*)new_page + blocksize; // nothing in this page
	new_page->page_number = i*blocksize + j + DATA_BEGIN;
	return NULL;
}

static void free_file_pages(filepage* head_page)
{
	if (!head_page)
		return;
	free_file_pages(head_page->nextpage);
	int d_block = head_page->page_number / blocksize;
	int d_number = head_page->page_number % blocksize;
	munmap(head_page, blocksize);
	*((char*)mem[BITMAP_BEGIN + INODE_BITMAP_SIZE + d_block] + d_number) = FREE;
	return;
}

static int create_filenode(const char *filename, const struct stat *st)
{
	inode *new_inode = creat_inode_sector(); // return a inode with inumber
	filepage *head_page = creat_filepage(&new_inode->head_page_number);
	new_inode->st = &new_inode->status;
	if (strlen(filename) <= LONGESTPATH) {
		memcpy(new_inode->filename, filename, strlen(filename) + 1);
	}
	else {
		return -ENAMETOOLONG;
	}
	memcpy(new_inode->st, st, sizeof(struct stat));

	inode* last_node;
	for (last_node = (inode*)mem[INODE_BEGIN]; last_node->next; last_node = last_node->next)
		;
	last_node->next = new_inode;
	new_inode->next = NULL;
}

static seek_tuple seek_offset(const char *path, off_t offset)
{
	inode* node = get_filenode(path);
	filepage *page = (filepage*)mem[node->head_page_number];
	size_t size = 0;
	while (size < offset) {
		size_t nextsize;
		nextsize = size + blocksize - ((char*)page->content - (char*)page);
		if (nextsize > offset) break;
		page = page->nextpage;
	}
	seek_tuple seek;
	seek.page = page;
	seek.page_offset = (char*)page->content + offset - size;
	return seek;
}

static void *oshfs_init(struct fuse_conn_info *conn)
{
	//initiate the superblock
	mem[0] = mmap(NULL, blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	//add some information here$

	//initiate the bitmap, including inode bitmap and databitmap
	for (int i = BITMAP_BEGIN; i < BITMAP_SIZE; i++) {
		mem[i] = mmap(NULL, blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		memset(mem[i], FREE, blocksize); // set all the bitmaps free
	}

	//initiate first inode block
	mem[INODE_BEGIN] = mmap(NULL, blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	char* i_bitmap = (char*)mem[BITMAP_BEGIN];
	i_bitmap[ROOTNUMBER] = 1; //root inode is allocated
	//d_bitmap[0] = 1;//root directory is allocated
	return NULL;
}

/*{
if ((strlen(path)) > LONGESTPATH) {
printf("path is too long!\n");
return NULL;
}
else if (path[0] != '/') {
printf("not a valid path!\n");
return NULL;
}
else {
char* current_path;
char path_temp[LONGESTPATH];
inode* creat_dir;
strcpy(path_temp, path);
current_path = path_temp + 1;
creat_dir = traverse_path(path_temp[1], rootptr, &current_path);
//char* first_dir_end;
//if (first_dir_end = strchr(current_path, '/')) { //if need make more than one directory
//	*first_dir_end = '\0';
//}
make_dir_info(creat_dir, current_path);
}
}
*/
static int oshfs_getattr(const char *path, struct stat *stbuf)
{
	int ret = 0;
	inode *node = get_filenode(path);
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

static int oshfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
	inode *node = (inode*)mem[INODE_BEGIN];
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	while (node) {
		filler(buf, node->filename, node->st, 0);
		node = node->next;
	}
	return 0;
}

static int oshfs_mknod(const char *path, mode_t mode, dev_t dev)
{
	struct stat st;
	st.st_mode = S_IFREG | 0644;
	st.st_uid = fuse_get_context()->uid;
	st.st_gid = fuse_get_context()->gid;
	st.st_nlink = 1;
	st.st_size = 0;
	create_filenode(path + 1, &st);
	return 0;
}

static int oshfs_open(const char *path, struct fuse_file_info *fi)
{
	return 0;
}


static int oshfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	inode *node = get_filenode(path);
	if (offset + size > node->st->st_size) {
		node->st->st_size = offset + size;
	}
	seek_tuple seek;
	seek = seek_offset(path, offset);
	free_file_pages(seek.page->nextpage);

	size_t remain = (char*)seek.page + blocksize - seek.page_offset;
	char *buf_temp = buf;

	if (remain >= size) {
		memcpy(seek.page_offset, buf, strlen(buf));
		return size;
	}
	else {
		memcpy(seek.page_offset, buf, remain);
		size = size - remain;
		buf_temp = buf_temp + remain;
	}

	filepage *page = seek.page;
	while (size > blocksize - sizeof(filepage)) {
		filepage *newpage = creat_filepage(NULL);
		newpage->content = (char*)newpage + sizeof(filepage);
		memcpy(newpage->content, buf, blocksize - sizeof(filepage));
		page->nextpage = newpage;
		newpage->nextpage = NULL;
		page = newpage;
		size = size - blocksize + sizeof(filepage);
		buf_temp = buf_temp + blocksize - sizeof(filepage);
	}
	filepage *newpage = creat_filepage(NULL);
	page->nextpage = newpage;
	newpage->nextpage = NULL;
	newpage->content = (char*)newpage + blocksize - size;
	memcpy(newpage->content, buf, size);
	return size;
}

static int oshfs_truncate(const char *path, off_t size)
{
	inode *node = get_filenode(path);
	if (node->st->st_size < size) {
		node->st->st_size = size;
	}
	seek_tuple seek;
	seek = seek_offset(path, size);
	filepage* overpage = seek.page->nextpage;
	free_file_pages(overpage);
	seek.page->nextpage = NULL;
	size_t last_content = seek.page_offset - (char*)seek.page->content;
	char buf[last_content + 1];
	memcpy(buf, seek.page->content, last_content);
	buf[last_content] = '\0';
	seek.page->content = (char*)seek.page + blocksize - last_content;
	memcpy(seek.page->content, buf, last_content);
	return 0;
}

static int oshfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	inode *node = get_filenode(path);
	seek_tuple seek = seek_offset(path, offset);
	int remain_size = size;
	int done_size = 0;
	if (size < ((char*)seek.page + blocksize) - seek.page_offset) {
		memcpy(buf, seek.page_offset, size);
		buf[size] = '\0';
		return size;
	}
	memcpy(buf, seek.page_offset, ((char*)seek.page + blocksize) - seek.page_offset);
	done_size = ((char*)seek.page + blocksize) - seek.page_offset;
	filepage* page = seek.page->nextpage;
	while (remain_size > ((char*)page + blocksize -(char*)page->content))
	{
		memcpy(buf + done_size, page->content, (char*)page + blocksize - (char*)page->content);
		done_size += (char*)page + blocksize - (char*)page->content;
		remain_size -= (char*)page + blocksize - (char*)page->content;
	}
	memcpy(buf + done_size, page->content, remain_size);
	return size;
}

static int oshfs_unlink(const char *path)
{
	inode *unlink_node = get_filenode(path);
	free_file_pages((filepage*)(mem + unlink_node->head_page_number));
	inode* node;
	inode* rootptr = (inode*)mem[INODE_BEGIN];
	if (unlink_node == rootptr) {
		rootptr = unlink_node->next;
	}
	else {
		for (node = rootptr; node->next != unlink_node; node = node->next)
			;
		node->next = node->next->next;
	}
	int bitmap_block = unlink_node->inum / blocksize;
	int bitmap_number = unlink_node->inum % blocksize;
	char* bitmap;
	bitmap = (char*)mem[BITMAP_BEGIN + bitmap_block];
	bitmap[bitmap_number] = FREE;
	int all_free = 1;
	for (int i = (bitmap_number / 8) * 8; i < (bitmap_number / 8) * 8 + 8; i++) {
		if (bitmap[i] == INUSE)
			all_free = 0;
	}
	if (all_free) munmap(mem[(unlink_node->inum / 8 + INODE_BEGIN)], blocksize);
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

int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &op, NULL);
}
