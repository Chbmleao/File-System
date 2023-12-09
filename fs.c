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
#define MAX_NAME 100 // File name max size
#define MAX_SUBFOLDERS 100 // Max number of subfolders in a file path
#define MAX_FILE_SIZE 5000 // Max number of blocks in a file
#define MAX_PATH_NAME 4000 // File path max size

// Permissions constants
#define RD_WR_OWNER S_IRUSR | S_IWUSR 
#define RD_WR_GROUP S_IRGRP | S_IWGRP
#define RD_WR_OTHER S_IROTH | S_IWOTH
#define RD_WR_ALL RD_WR_OWNER | RD_WR_GROUP | RD_WR_OTHER

void copyInode(struct inode *src, struct inode *dest, struct nodeinfo *srcNodeInfo){
	if(src->mode == IMDIR){
		for(int i = 0; i < srcNodeInfo->size; i++) 
			dest->links[i] = src->links[i];
	}
	dest->meta = src->meta;
	dest->next = src->next;
	dest->parent = src->parent;
	dest->mode = src->mode;
}

void copyInodeInfo(struct nodeinfo *src, struct nodeinfo *dest){
	dest->size = src->size;
	for(int i = 0; i < 7; i++)
		dest->reserved[i] = src->reserved[i];
	
	strcat(dest->name, src->name);
}

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

  struct inode *fileParentINode;
  struct nodeinfo *fileParentNodeInfo;
  copyInode(root_inode, fileParentINode, root_node_info);
  copyInodeInfo(root_node_info, fileParentNodeInfo);

  uint64_t parentOfFileParentINodeIdx = (uint64_t) 1;
  uint64_t parentOfFileParentNodeInfoIdx = (uint64_t) 2;

  uint64_t is_new_file = 0;
  struct inode *previousInode;
  struct inode *currentInode = (struct inode*) malloc(sb->blksz);
  copyInode(root_inode, previousInode, root_node_info);

  struct nodeinfo *previousNodeInfo;
  struct nodeinfo *currentNodeInfo = (struct nodeinfo*) malloc(sb->blksz);
  copyInodeInfo(root_node_info, previousNodeInfo);

  free(root_inode);
  free(root_node_info);

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
          copyInode(currentInode, fileParentINode, currentNodeInfo);
          copyInodeInfo(currentNodeInfo, fileParentNodeInfo);

          parentOfFileParentINodeIdx = previousInode->links[j];
          parentOfFileParentNodeInfoIdx = currentInode->meta;
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
    copyInode(currentInode, previousInode, currentNodeInfo);
    copyInodeInfo(currentNodeInfo, previousNodeInfo);
  }

  int blocks_needed;
  if(is_new_file) {
    // Stores the new iNode as a child of the parent node
    fileParentINode->links[fileParentNodeInfo->size] = block_indexes[1];
    fileParentNodeInfo->size += 1;

    // Gets amount of blocks needed to store the file
    blocks_needed = cnt/(sb->blksz-20); //TODO: attention

    // Gets free blocks to store the file
    for(int i = 0; i < blocks_needed-1; i++) {
      block_indexes[i+2] = fs_get_block(sb);
    }
  } else {
    // Get amount of blocks already used by the file node
    int blocks_currently_being_used = previousNodeInfo->size/(sb->blksz-20); //TODO: attention
  
    // Gets amount of blocks needed to store the file
    blocks_needed = cnt/(sb->blksz-20); //TODO: attention

    if(blocks_currently_being_used <= blocks_needed) {
      // There are not enough or just enough blocks to store the new content
      int block_idx = 2; // first 2 position of block_indexes already used
      copyInode(previousInode, currentInode, previousNodeInfo);
      // Maps the blocks already being used to the block_indexes vector
      while(currentInode->next != 0) {
        block_indexes[block_idx] = previousInode->next; // all already being used
        lseek(sb->fd, previousInode->next*sb->blksz, SEEK_SET);
        read(sb->fd, currentInode, sb->blksz);

        block_idx++;
      }

      // Gets free blocks to store the file
      for(int i = blocks_currently_being_used + 1; i <= blocks_needed; i++) { //TODO: try to change blocks_used => block idx
        block_indexes[i] = fs_get_block(sb);
      }
    } else {
      // There are more than enough blocks to store the new content
      int block_idx = 2; // first 2 position of block_indexes already used
      copyInode(previousInode, currentInode, previousNodeInfo);
      // Maps the blocks already being used to the block_indexes vector
      while(currentInode->next != 0) {
        block_indexes[block_idx] = previousInode->next; // all already being used
        lseek(sb->fd, previousInode->next*sb->blksz, SEEK_SET);
        read(sb->fd, currentInode, sb->blksz);

        block_idx++;
      }

      // Deallocates the blocks that will not be used anymore
      for(int i = blocks_currently_being_used; i > blocks_currently_being_used; i--) {
        int fs_put_block_output = fs_put_block(sb, block_indexes[i]);
        if(fs_put_block_output != 0) return fs_put_block_output;
      }
    }
  }

  // Update the parent nodeinfo of the file node
  lseek(sb->fd, parentOfFileParentNodeInfoIdx*sb->blksz, SEEK_SET);
  write(sb->fd, fileParentNodeInfo, sb->blksz);

  // Update the parent inode of the file node
  lseek(sb->fd, parentOfFileParentINodeIdx*sb->blksz, SEEK_SET);
  write(sb->fd, fileParentINode, sb->blksz);

  // Update the file nodeinfo
  lseek(sb->fd, block_indexes[0]*sb->blksz, SEEK_SET);
  write(sb->fd, currentNodeInfo, sb->blksz);

  // Update the file inode
  previousInode->mode = IMREG;
  previousInode->meta = block_indexes[0];
  previousInode->parent = block_indexes[1];

  if(blocks_needed == 1) {
    // The file fits in a single block
    previousInode->next = 0;
  } else {
    // The file does not fit in a single block
    previousInode->next = block_indexes[2];
  }

  lseek(sb->fd, block_indexes[1]*sb->blksz, SEEK_SET);
  write(sb->fd, previousInode, sb->blksz);

  // Write the file content on children blocks
  copyInode(previousInode, currentInode, previousNodeInfo);
  currentInode->mode = IMCHILD;
  currentInode->parent = block_indexes[1];

  // Write buffer content on all inodes
  for(int i = 2; i <= blocks_needed; i++) {
    currentInode->meta = block_indexes[i-1];
    if(i == blocks_needed) {
      // Last block
      currentInode->next = 0;
    } else currentInode->next = block_indexes[i+1];

    lseek(sb->fd, block_indexes[i]*sb->blksz, SEEK_SET);
    write(sb->fd, currentInode, sb->blksz);

    // Write buffer content on the current inode
    lseek(sb->fd, block_indexes[i]*sb->blksz + 20, SEEK_SET);
    write(sb->fd, buf, sb->blksz - 20);

    // Update buffer pointer
    buf += sb->blksz - 20; //TODO: attention
  }

  // Writes superblock to file
  lseek(sb->fd, 0, SEEK_SET);
  if(write(sb->fd, sb, sb->blksz) < 0) {
    // Error writing superblock
    errno = EPERM;
    return -1;
  }

  return 0;
}

ssize_t fs_read_file(struct superblock *sb, const char *fname, char *buf, size_t bufsz) {

}

int fs_unlink(struct superblock *sb, const char *fname) {
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

  struct inode *fileParentINode;
  struct nodeinfo *fileParentNodeInfo;
  copyInode(root_inode, fileParentINode, root_node_info);
  copyInodeInfo(root_node_info, fileParentNodeInfo);

  uint64_t parentOfFileParentINodeIdx = (uint64_t) 1;
  uint64_t parentOfFileParentNodeInfoIdx = (uint64_t) 2;

  struct inode *previousInode;
  struct inode *currentInode = (struct inode*) malloc(sb->blksz);
  copyInode(root_inode, previousInode, root_node_info);

  struct nodeinfo *previousNodeInfo;
  struct nodeinfo *currentNodeInfo = (struct nodeinfo*) malloc(sb->blksz);
  copyInodeInfo(root_node_info, currentNodeInfo);

  free(root_inode);
  free(root_node_info);

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
        } else {
          parentOfFileParentINodeIdx = previousInode->links[j];
          parentOfFileParentNodeInfoIdx = currentInode->meta;
        }
        break;
      } else if(j == (path_depth-1)  || previousInode->next == 0) {
        // Given folder path does not exists
        errno = ENOENT;
        return -1;
      }

      // Read the next inode
      lseek(sb->fd, previousInode->next*sb->blksz, SEEK_SET);
      read(sb->fd, previousInode, sb->blksz);
    }
    // Updates the inode parents
    copyInode(currentInode, previousInode, currentNodeInfo);
    copyInodeInfo(currentNodeInfo, previousNodeInfo);

    // Read the next folder
    copyInode(currentInode, previousInode, currentNodeInfo);
    copyInodeInfo(currentNodeInfo, previousNodeInfo);
  }

  // Remove parent references to the removed blocks
  for(int i = 0; i < fileParentNodeInfo->size; i++) {
    if(fileParentINode->links[i] == block_indexes[1]) {
      for(int j = i; j < fileParentNodeInfo->size - 1; j++) {
        fileParentINode->links[j] = fileParentINode->links[j+1];
      }
      break;
    }
  }

  // Put removed blocks back to the free list
	fs_put_block(sb, block_indexes[0]);
	fs_put_block(sb, block_indexes[1]);

  // Update the parent nodeinfo of the file node
  fileParentNodeInfo->size -= 1;
  lseek(sb->fd, parentOfFileParentNodeInfoIdx*sb->blksz, SEEK_SET);
  write(sb->fd, fileParentNodeInfo, sb->blksz);

  // Update the parent inode of the file node
  lseek(sb->fd, parentOfFileParentINodeIdx*sb->blksz, SEEK_SET);
  write(sb->fd, fileParentINode, sb->blksz);

  // Update superblock on the file
  lseek(sb->fd, 0, SEEK_SET);
  if(write(sb->fd, sb, sb->blksz) < 0) {
    // Error writing superblock
    errno = EPERM;
    return -1;
  }

  return 0;
}

int fs_mkdir(struct superblock *sb, const char *dname) {
  struct inode *inode1 = (struct inode*) malloc(sb->blksz);
  struct nodeinfo *nodeInfo1 = (struct nodeinfo*) malloc(sb->blksz);
  struct inode *inode2 = (struct inode*) malloc(sb->blksz);
  struct nodeinfo *nodeInfo2 = (struct nodeinfo*) malloc(sb->blksz);

  int i = 0;
  char *token = strtok(dname, "/");
  char files[MAX_SUBFOLDERS][MAX_NAME];
  while(token != NULL){
    strcpy(files[i], token);
    token = strtok(NULL, "/");
    i++;
  }
  int numSubfolders = i;

  // Get the root directory iNode
  lseek(sb->fd, sb->blksz * sb->root, SEEK_SET);
  read(sb->fd, inode1, sb->blksz);

  // Get the root directory nodeinfo
  lseek(sb->fd, sb->blksz * 1, SEEK_SET);
  read(sb->fd, nodeInfo1, sb->blksz);

  uint64_t blocks[MAX_FILE_SIZE];
  blocks[0] = 1;
  blocks[1] = 2; 

  // Search for the file in using the path trough root directory
  for (int j = 0; j < numSubfolders; j++) {
    // Infinite loop until check all the subpaths in the path
    while(1) {
      int found = 0;
      // Check if the current iNode is a subfolder or file
      int k;
      for(k = 0; k < nodeInfo1->size; k++) {
        // Read the inode from the file
        lseek(sb->fd, inode1->links[k] * sb->blksz, SEEK_SET);
        read(sb->fd, inode2, sb->blksz);

        // Deal with the case where current is child node
        if(inode2->mode == IMCHILD) {
          lseek(sb->fd, inode2->parent * sb->blksz, SEEK_SET);
          read(sb->fd, inode2, sb->blksz);
        }

        // Get's nodeinfo from the inode
        lseek(sb->fd, inode2->meta * sb->blksz, SEEK_SET);
        read(sb->fd, nodeInfo2, sb->blksz);

        // Check by the path name if it matches with the current nodeinfo
        if(strcmp(nodeInfo2->name, files[j]) == 0) {
          found = 1;
          break;
        }
      }
    
      if (found == 1) {
        // The current node is the subfolder or file we are looking for
        if (j == numSubfolders-2) {
          blocks[0] = inode2->meta;
          blocks[1] = inode1->links[k];
        }
        break;
      } else if (j == numSubfolders - 1 || inode1->next == 0) {
        // The directory does not exists
        errno = ENOENT;
        return -1;
      } 

      lseek(sb->fd, inode1->next * sb->blksz, SEEK_SET);
      read(sb->fd, inode1, sb->blksz);
    }

    // Read the next folder
    copyInode(inode2, inode1, nodeInfo2);
    copyInodeInfo(nodeInfo2, nodeInfo1);
  }

  // Stores the new iNode nodeinfo
  uint64_t block_info = fs_get_block(sb);
  nodeInfo2->size = 0;
  strcat(nodeInfo2->name, files[numSubfolders-1]);

  // Write the nodeinfo
  lseek(sb->fd, block_info * sb->blksz, SEEK_SET);
  write(sb->fd, nodeInfo2, sb->blksz);

  // Stores the new iNode
  uint64_t block_inode = fs_get_block(sb);
  inode2->mode = IMDIR;
  inode2->parent = blocks[1];
  inode2->meta = block_info;
  inode2->next = 0;

  // Write the inode
  lseek(sb->fd, block_inode * sb->blksz, SEEK_SET);
  write(sb->fd, inode2, sb->blksz);

  // Stores the new iNode as a child of the parent node
  inode1->links[nodeInfo1->size] = block_inode;
  nodeInfo1->size += 1;

  // Write the inode
  lseek(sb->fd, blocks[1] * sb->blksz, SEEK_SET);
  write(sb->fd, inode1, sb->blksz);

  // Write the nodeinfo
  lseek(sb->fd, blocks[0] * sb->blksz, SEEK_SET);
  write(sb->fd, nodeInfo1, sb->blksz);

  // Updates superblock
  lseek(sb->fd, 0, SEEK_SET);
  write(sb->fd, sb, sb->blksz);

  return 0;
}

int fs_rmdir(struct superblock *sb, const char *dname) {
  struct inode *inode1 = (struct inode*) malloc(sb->blksz);
  struct nodeinfo *nodeInfo1 = (struct nodeinfo*) malloc(sb->blksz);
  struct inode *inode2 = (struct inode*) malloc(sb->blksz);
  struct nodeinfo *nodeInfo2 = (struct nodeinfo*) malloc(sb->blksz);
  struct inode *inodeParent = (struct inode*) malloc(sb->blksz);
  struct nodeinfo *nodeInfoParent = (struct nodeinfo*) malloc(sb->blksz);

  int i = 0;
  char *token = strtok(dname, "/");
  char files[MAX_SUBFOLDERS][MAX_NAME];
  while(token != NULL){
    strcpy(files[i], token);
    token = strtok(NULL, "/");
    i++;
  }
  int numSubfolders = i;

  // Get the root directory iNode
  lseek(sb->fd, sb->blksz * sb->root, SEEK_SET);
  read(sb->fd, inode1, sb->blksz);

  // Get the root directory nodeinfo
  lseek(sb->fd, sb->blksz * 1, SEEK_SET);
  read(sb->fd, nodeInfo1, sb->blksz);

  uint64_t blocks[MAX_FILE_SIZE];
  blocks[0] = 1;
  blocks[1] = 2; 

  int parentInfoRoot = 1;
  int parentInodeRoot = 2;

  // Search for the file in using the path trough root directory
  for (int j = 0; j < numSubfolders; j++) {
    // Infinite loop until check all the subpaths in the path
    while(1) {
      int found = 0;
      // Check if the current iNode is a subfolder or file
      int k;
      for(k = 0; k < nodeInfo1->size; k++) {
        // Read the inode from the file
        lseek(sb->fd, inode1->links[k] * sb->blksz, SEEK_SET);
        read(sb->fd, inode2, sb->blksz);

        // Deal with the case where current is child node
        if(inode2->mode == IMCHILD) {
          lseek(sb->fd, inode2->parent * sb->blksz, SEEK_SET);
          read(sb->fd, inode2, sb->blksz);
        }

        // Get's nodeinfo from the inode
        lseek(sb->fd, inode2->meta * sb->blksz, SEEK_SET);
        read(sb->fd, nodeInfo2, sb->blksz);

        // Check by the path name if it matches with the current nodeinfo
        if(strcmp(nodeInfo2->name, files[j]) == 0) {
          found = 1;
          break;
        }
      }
    
      if (found == 1) {
        // The current node is the subfolder or file we are looking for
        if (j == numSubfolders-1) {
          blocks[0] = inode2->meta;
          blocks[1] = inode1->links[k];
        } else {
          parentInfoRoot = inode2->meta;
          parentInodeRoot = inode1->links[k];
        }
        break;
      } else if (j == numSubfolders - 1 || inode1->next == 0) {
        // The directory does not exists
        errno = ENOENT;
        return -1;
      } 

      lseek(sb->fd, inode1->next * sb->blksz, SEEK_SET);
      read(sb->fd, inode1, sb->blksz);
    }

    copyInode(inode1, inodeParent, nodeInfo1);
    copyInodeInfo(nodeInfo1, nodeInfoParent);

    // Read the next folder
    copyInode(inode2, inode1, nodeInfo2);
    copyInodeInfo(nodeInfo2, nodeInfo1);
  }
  
  // Check if the directory is empty
  if(nodeInfo1->size > 0) {
    errno = ENOTEMPTY;
    return -1;
  }

  // Free blocks used by the directory
  fs_put_block(sb, blocks[0]);
  fs_put_block(sb, blocks[1]);

  // Remove the directory from the parent node
  for(int i = 0; i < nodeInfoParent->size; i++) {
    if(inodeParent->links[i] == blocks[1]) {
      while (i < nodeInfoParent->size - 1) {
        inodeParent->links[i] = inodeParent->links[i+1];
        i++;
      }
      break;    
    }
  }

  nodeInfoParent->size -= 1;

  // Write the inode
  lseek(sb->fd, parentInodeRoot * sb->blksz, SEEK_SET);
  write(sb->fd, inodeParent, sb->blksz);

  // Write the nodeinfo
  lseek(sb->fd, parentInfoRoot * sb->blksz, SEEK_SET);
  write(sb->fd, nodeInfoParent, sb->blksz);

  // Updates superblock
  lseek(sb->fd, 0, SEEK_SET);
  write(sb->fd, sb, sb->blksz);

  return 0;
}

char * fs_list_dir(struct superblock *sb, const char *dname) {
  struct inode *inode1 = (struct inode*) malloc(sb->blksz);
  struct nodeinfo *nodeInfo1 = (struct nodeinfo*) malloc(sb->blksz);
  struct inode *inode2 = (struct inode*) malloc(sb->blksz);
  struct nodeinfo *nodeInfo2 = (struct nodeinfo*) malloc(sb->blksz);

  // TODO: verify if this works
  // char * name = (char*) malloc(MAX_PATH_NAME*sizeof(char));
	// strcpy(name, dname);


  // Get a vector of string with the path levels
  int i = 0;
  char files[MAX_SUBFOLDERS][MAX_NAME];
  char *token = strtok(dname, "/");
  while(token != NULL){
    strcpy(files[i], token);
    token = strtok(NULL, "/");
    i++;
  }
  int numSubfolders = i;

  // Get the root directory iNode
  lseek(sb->fd, sb->blksz * sb->root, SEEK_SET);
  read(sb->fd, inode1, sb->blksz);

  // Get the root directory nodeinfo
  lseek(sb->fd, sb->blksz * 1, SEEK_SET);
  read(sb->fd, nodeInfo1, sb->blksz);

  // Search for the file in using the path trough root directory
  for (int j = 0; j < numSubfolders; j++) {
    // Infinite loop until check all the subpaths in the path
    while(1) {
      int found = 0;
      // Check if the current iNode is a subfolder or file
      int k;
      for(k = 0; k < nodeInfo1->size; k++) {
        // Read the inode from the file
        lseek(sb->fd, inode1->links[k] * sb->blksz, SEEK_SET);
        read(sb->fd, inode2, sb->blksz);

        // Deal with the case where current is child node
        if(inode2->mode == IMCHILD) {
          lseek(sb->fd, inode2->parent * sb->blksz, SEEK_SET);
          read(sb->fd, inode2, sb->blksz);
        }

        // Get's nodeinfo from the inode
        lseek(sb->fd, inode2->meta * sb->blksz, SEEK_SET);
        read(sb->fd, nodeInfo2, sb->blksz);

        // Check by the path name if it matches with the current nodeinfo
        if(strcmp(nodeInfo2->name, files[j]) == 0) {
          found = 1;
          break;
        }
      }
    
      if (found == 1) {
        // The current node is the subfolder or file we are looking for
        break;
      } else if (j == (numSubfolders - 1) || inode1->next == 0) {
        // The directory does not exists
        errno = ENOENT;
        char *elements = (char*) malloc(3 * sizeof(char));
        strcat(elements, "-1"); // TODO: change this to test if works
        return elements;
      } 

      lseek(sb->fd, inode1->next * sb->blksz, SEEK_SET);
      read(sb->fd, inode1, sb->blksz);
    }

    // Read the next folder
    copyInode(inode2, inode1, nodeInfo2);
    copyInodeInfo(nodeInfo2, nodeInfo1);
  }

  char *list = (char*) malloc(nodeInfo1->size * sizeof(char));

  // Read the nodeinfo of the subfolder
  for(int j = 0; j < nodeInfo1->size; j++) {
    // Read the inode from the file
    lseek(sb->fd, inode1->links[j] * sb->blksz, SEEK_SET);
    read(sb->fd, inode2, sb->blksz);

    // Deal with the case where current is child node
    if(inode2->mode == IMCHILD) {
      lseek(sb->fd, inode2->parent * sb->blksz, SEEK_SET);
      read(sb->fd, inode2, sb->blksz);
    }

    // Get's nodeinfo from the inode
    lseek(sb->fd, inode2->meta * sb->blksz, SEEK_SET);
    read(sb->fd, nodeInfo2, sb->blksz);

    // Concatenate the name of the file to the list string
    strcat(list, nodeInfo2->name);
    
    // If current is a subfolder, add a '/' to the end of the name
    if(inode2->mode == IMDIR) {
      strcat(list, "/");
    }

    if (j < nodeInfo1->size - 1) {
      strcat(list, " ");
    }
  }

  return list;
}