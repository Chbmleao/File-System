#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <unistd.h>

#include "fs.h"

#define FS_MAGIC_NUMBER 0xdcc605f5

// Permissions constants
#define RD_WR_OWNER S_IRUSR | S_IWUSR 
#define RD_WR_GROUP S_IRGRP | S_IWGRP
#define RD_WR_OTHER S_IROTH | S_IWOTH
#define RD_WR_ALL RD_WR_OWNER | RD_WR_GROUP | RD_WR_OTHER

struct superblock * fs_format(const char *fname, uint64_t blocksize) {
  long int size_bytes;
  FILE* storage;

  // Block needs to be at least 128 bytes to store the superblock
  if(blocksize < MIN_BLOCK_SIZE){
		errno = EINVAL;
		return NULL;
	}

  // Open file
  storage = fopen(fname, "r+"); //TODO: attention
  if(storage == NULL){
    errno = ENOENT;
    return NULL;
  }

  // Get file size
  fseek(storage, 0L, SEEK_END);
  size_bytes = ftell(storage);

  // Closes file since we'll proceed with the usage of linux default API to deal with the file
  fclose(storage);

  // Check if file is big enough
  if(size_bytes < MIN_BLOCK_COUNT * blocksize){
    errno = ENOSPC;
    return NULL;
  }

  // Allocate superblock space
  struct superblock *sb = (struct superblock*) malloc(blocksize); // TODO: try to change
  if(sb == NULL){
    errno = ENOSPC;
    return NULL;
  }

  // Initialize superblock
  sb->magic = FS_MAGIC_NUMBER;
  sb->blks = size_bytes/blocksize;
  sb->blksz = blocksize;

  sb->freeblks = sb->blks;
  sb->freeblks -= 1; // Superblock
  sb->freeblks -= 1; // root nodeinfo
  sb->freeblks -= 1; // root inode

  sb->root = 2; // root inode index
  sb->freelist = 3; // first free block index

  // Open file with read and write permissions using linux default API
  sb->fd = open(fname, O_RDWR, RD_WR_ALL); //TODO: try to change

  if(sb->fd < 0) {
    // Error opening file
    free(sb);
    errno = EBADF;
    return NULL;
  }

  // Write superblock to file
  if(write(sb->fd, sb, blocksize) < 0){
    // Error writing superblock
    close(sb->fd);
    free(sb);
    errno = EPERM;
    return NULL;
  }

  // Sets the infor to root directory
  struct nodeinfo *node_info = (struct nodeinfo*) malloc(blocksize);
  if(node_info == NULL){
    errno = ENOSPC;
    return NULL;
  }

  node_info->size = 0;
  // Sets the name of the root directory \0 is the end of the string
  strcpy(node_info->name, "/\0"); // TODO: try to change
  write(sb->fd, node_info, blocksize);
  free(node_info);

  // Sets the inode to root directory
  struct inode *iNode = (struct inode*) malloc(blocksize);
  if(iNode == NULL){
    errno = ENOSPC;
    return NULL;
  }

  // Fulfill the iNode fields
  iNode->mode = IMDIR;
  iNode->parent = 2; // root directory iNode index
  iNode->meta = 1; // root directory nodeinfo index
  iNode->next = 0; // no next iNode
  write(sb->fd, iNode, blocksize);
  free(iNode);

  struct freepage free_page;

  // Fulfill remaining blocks with free pages
  for(int i = 3; i < sb->blks - 1; i++){
    free_page.next = i + 1;
    free_page.count = 0;
    write(sb->fd, &free_page, blocksize);
  }

  // Last block is a free page with no next
  free_page.next = 0;
  free_page.count = 0;
  write(sb->fd, &free_page, blocksize);

  return sb;
}

struct superblock * fs_open(const char *fname) {
  int file_descriptor = open(fname, O_RDWR, RD_WR_ALL); // TODO: attention

  if(file_descriptor < 0) {
    // Error opening file
    errno = EBADF;
    return NULL;
  }

  // Prevents other processes from accessing the file
  if(flock(file_descriptor, LOCK_EX) < 0) {
    // Error locking file
    close(file_descriptor);
    errno = EBUSY;
    return NULL;
  }

  struct superblock *sb = (struct superblock*) malloc(sizeof(struct superblock));
  lseek(file_descriptor, 0, SEEK_SET); // TODO: try to change

  // Read superblock from file
  if(read(file_descriptor, sb, sizeof(struct superblock)) < 0){
    // Error reading superblock
    close(file_descriptor);
    free(sb);
    errno = EPERM;
    return NULL;
  }

  // Check by superblock if the file is from dcc605 file system
  if(sb->magic != FS_MAGIC_NUMBER){
    // File is not from dcc605 file system
    flock(file_descriptor, LOCK_NB | LOCK_UN); // TODO: try to change
    close(file_descriptor);
    free(sb);
    errno = EBADF;
    return NULL;
  }

  return sb;
}

int fs_close(struct superblock *sb) {
  if(sb == NULL) {
    // Superblock is NULL
    errno = EINVAL;
    return -1;
  }

  if(sb->magic != FS_MAGIC_NUMBER) {
    // Superblock is not from dcc605 file system
    errno = EBADF;
    return -1;
  }

  // Unlock file
  if(flock(sb->fd, LOCK_NB | LOCK_UN) < 0) { //Todo: try to change
    // Error unlocking file
    errno = EBUSY;
    return -1;
  }

  // Close file
  if(close(sb->fd) < 0) {
    // Error closing file
    errno = EPERM;
    return -1;
  }

  free(sb);

  return 0;
}

uint64_t fs_get_block(struct superblock *sb) {
  if(sb == NULL) {
    // Superblock is NULL
    errno = EINVAL;
    return (uint64_t) 0;
  }

  if(sb->magic != FS_MAGIC_NUMBER) {
    // Superblock is not from dcc605 file system
    errno = EBADF;
    return (uint64_t) 0;
  }

  if(sb->freeblks == 0) {
    // There are no free blocks
    errno = ENOSPC;
    return (uint64_t) 0;
  }

  // Uses seek to set the file pointer to the first free block
  lseek(sb->fd, sb->freelist * sb->blksz, SEEK_SET);
  struct freepage *first_fp = (struct freepage*) malloc(sb->blksz);

  // Read the first free block
  if(read(sb->fd, first_fp, sb->blksz) < 0) {
    // Error reading first free block
    free(first_fp);
    errno = ENOENT;
    return (uint64_t) 0; //TODO: attention
  }

  // Gets the first free block address
  uint64_t free_block = sb->freelist;

  // Updates superblock
  sb->freelist = first_fp->next;
  sb->freeblks -= 1;

  free(first_fp);

  // Updates superblock in file
  lseek(sb->fd, 0, SEEK_SET);
  if(write(sb->fd, sb, sb->blksz) < 0) {
    // Error writing superblock
    errno = EPERM;
    return (uint64_t) 0;
  }

  return free_block;
}

int fs_put_block(struct superblock *sb, uint64_t block) {
  if(sb == NULL) {
    // Superblock is NULL
    errno = EINVAL;
    return -1;
  }

  if(sb->magic != FS_MAGIC_NUMBER) {
    // Superblock is not from dcc605 file system
    errno = EBADF;
    return -1;
  }

  if(block <= 0) {
    // Block is invalid
    errno = EINVAL;
    return -1;
  }

  struct freepage *free_page = (struct freepage*) malloc(sb->blksz);
  
  // Block will be put in the first free block
  free_page->next = sb->freelist;
  sb->freelist = block;
  sb->freeblks += 1;

  // Updates superblock in file
  lseek(sb->fd, 0, SEEK_SET);
  if(write(sb->fd, sb, sb->blksz) < 0) {
    // Error writing superblock
    errno = EPERM;
    return -1;
  }

  // Adds the block to the file
  lseek(sb->fd, block * sb->blksz, SEEK_SET);
  if(write(sb->fd, free_page, sb->blksz) < 0) {
    // Error writing free page
    errno = EPERM;
    return -1;
  }

  return 0;
}

int fs_write_file(struct superblock *sb, const char *fname, char *buf, size_t cnt) {
  // Get a vector of string with the path levels
  int path_depth = 0;
  char path_vector[100][100];

  char *token = strtok(fname, "/");
  while(token != NULL){
    strcpy(path_vector[path_depth], token);
    token = strtok(NULL, "/");
    path_depth++;
  }

  // Get the root directory nodeinfo
  struct nodeinfo *root_node_info = (struct nodeinfo*) malloc(sb->blksz);
  int root_nodeinfo_index = 1;
  lseek(sb->fd, root_nodeinfo_index * sb->blksz, SEEK_SET);
  read(sb->fd, root_node_info, sb->blksz);

  // Get the root directory inode
  struct inode *root_inode = (struct inode*) malloc(sb->blksz);
  lseek(sb->fd, sb->root * sb->blksz, SEEK_SET);
  read(sb->fd, root_inode, sb->blksz);

  // Initialize the vector of block indexes
  uint64_t block_indexes[5000];
  block_indexes[0] = 1; // root nodeinfo index
  block_indexes[1] = sb->root; // root inode index

  struct inode *fileParentINode = root_inode;
  struct nodeinfo *fileParentNodeInfo = root_node_info;

  uint64_t is_new_file = 0;
  struct inode *previousInode = root_inode;
  struct inode *currentInode = (struct inode*) malloc(sb->blksz);

  struct nodeinfo *previousNodeInfo = root_node_info;
  struct nodeinfo *currentNodeInfo = (struct nodeinfo*) malloc(sb->blksz);

  int subpath_exists = 0;
  // Search for the file in using the path trough root directory
  for(int i = 0; i < path_depth; i++) {
    // Infinite loop until check all the subpaths in the path
    while(1) {
      subpath_exists = 0;
      int j;
      for(j = 0; j < root_node_info->size; j++) {
        // Read the inode from the file
        lseek(sb->fd, previousInode->links[j]*sb->blksz, SEEK_SET);
        read(sb->fd, currentInode, sb->blksz);

        // Deal with the case where current is child node
        if(currentInode->mode == IMCHILD) {
          lseek(sb->fd, currentInode->parent*sb->blksz, SEEK_SET);
          read(sb->fd, currentInode, sb->blksz);
        }

        // Get's nodeinfo from the inode
        lseek(sb->fd, currentInode->meta*sb->blksz, SEEK_SET);
        read(sb->fd, currentNodeInfo, sb->blksz);

        // Check by the path name if it matches with the current nodeinfo
        if(strcmp(currentNodeInfo->name, path_vector[i]) == 0){
					subpath_exists = 1;
					break;
				}
      }
    
      if(subpath_exists == 1) {
        // The current node is a subfolder or file
        if(i == (path_depth - 1)) {
          // The current node is the file
          block_indexes[0] = currentInode->meta;
          block_indexes[1] = previousInode->links[j];
          root_node_info->size += 1;
          is_new_file = 0;
        } else {
          // Updates the inode parents
          fileParentINode = currentInode;
          fileParentNodeInfo = currentNodeInfo;
        }
        break;
      } else {
        // Current node does no point to the file nor a subfolder
        if(i == (path_depth - 1)) {
          // File does not exists. Create it

          // Store info of new node
          strcpy(currentNodeInfo->name, path_vector[i]);
          currentNodeInfo->size = sb->blksz - 20;

          // Store it in a free block
          block_indexes[0] = fs_get_block(sb); //TODO: attention

          // Store info of new inode
          currentInode->mode = IMREG;
          currentInode->parent = block_indexes[1];
          currentInode->meta = block_indexes[0];
          currentInode->next = 0;

          // Store it in a free block
          block_indexes[1] = fs_get_block(sb); //TODO: attention

          is_new_file =1;
        } else if(previousInode->next == 0) {
          // Given folder path does not exists
          errno = ENOENT;
          return -1;
        }
        break;
      }

      // Read the next inode
      lseek(sb->fd, previousInode->next*sb->blksz, SEEK_SET);
      read(sb->fd, previousInode, sb->blksz);
    }

    // Read the next folder
    previousInode = currentInode;
    previousNodeInfo = currentNodeInfo;
  }

  if(is_new_file) {
    // Stores the new iNode as a child of the parent node
    fileParentINode->links[fileParentNodeInfo->size] = block_indexes[1];
    fileParentNodeInfo->size += 1;

    // Gets amount of blocks needed to store the file
    int blocks_needed = cnt/(sb->blksz-20); //TODO: attention

    // Gets free blocks to store the file
    for(int i = 0; i < blocks_needed-1; i++) {
      block_indexes[i+2] = fs_get_block(sb);
    }
  } else {

  }
}

ssize_t fs_read_file(struct superblock *sb, const char *fname, char *buf, size_t bufsz) {

}

int fs_unlink(struct superblock *sb, const char *fname) {

}

int fs_mkdir(struct superblock *sb, const char *dname) {

}

int fs_rmdir(struct superblock *sb, const char *dname) {

}

char * fs_list_dir(struct superblock *sb, const char *dname) {
  
}