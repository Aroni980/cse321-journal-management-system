#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FS_MAGIC 0x56534653U
#define JOURNAL_MAGIC 0x4A524E4CU

#define BLOCK_SIZE        4096U
#define INODE_SIZE         128U
#define JOURNAL_BLOCK_IDX    1U
#define JOURNAL_BLOCKS      16U
#define INODE_BLOCKS         2U
#define DATA_BLOCKS         64U
#define INODE_BMAP_IDX     (JOURNAL_BLOCK_IDX + JOURNAL_BLOCKS)
#define DATA_BMAP_IDX      (INODE_BMAP_IDX + 1U)
#define INODE_START_IDX    (DATA_BMAP_IDX + 1U)
#define DATA_START_IDX     (INODE_START_IDX + INODE_BLOCKS)
#define TOTAL_BLOCKS       (DATA_START_IDX + DATA_BLOCKS)
#define DIRECT_POINTERS     8U
#define DEFAULT_IMAGE "vsfs.img"

#define REC_DATA   1
#define REC_COMMIT 2

struct superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;

    uint32_t journal_block;
    uint32_t inode_bitmap;
    uint32_t data_bitmap;
    uint32_t inode_start;
    uint32_t data_start;

    uint8_t  _pad[128 - 9 * 4];
};

struct inode {
    uint16_t type;
    uint16_t links;
    uint32_t size;

    uint32_t direct[DIRECT_POINTERS];

    uint32_t ctime;
    uint32_t mtime;

    uint8_t _pad[128 - (2 + 2 + 4 + DIRECT_POINTERS * 4 + 4 + 4)];
};

struct dirent {
    uint32_t inode;
    char name[28];
};

struct journal_header {
    uint32_t magic;
    uint32_t nbytes_used;
};

struct rec_header {
    uint16_t type;
    uint16_t size;
};

struct data_record {
    struct rec_header hdr;
    uint32_t block_no;
    uint8_t data[4096];
};

_Static_assert(sizeof(struct superblock) == 128, "superblock must be 128 bytes");
_Static_assert(sizeof(struct inode) == 128, "inode must be 128 bytes");
_Static_assert(sizeof(struct dirent) == 32, "dirent must be 32 bytes");
_Static_assert(sizeof(struct journal_header) == 8, "journal_header must be 8 bytes");
_Static_assert(sizeof(struct rec_header) == 4, "rec_header must be 4 bytes");
_Static_assert(sizeof(struct data_record) == 4104, "data_record must be 4104 bytes");

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void read_block(int fd, uint32_t block_index, void *buf) {
    off_t offset = (off_t)block_index * BLOCK_SIZE;
    ssize_t n = pread(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE) {
        die("pread");
    }
}

static void write_block(int fd, uint32_t block_index, const void *buf) {
    off_t offset = (off_t)block_index * BLOCK_SIZE;
    ssize_t n = pwrite(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE) {
        die("pwrite");
    }
}

static int bitmap_test(const uint8_t *bitmap, uint32_t index) {
    return (bitmap[index / 8] >> (index % 8)) & 0x1;
}

static void bitmap_set(uint8_t *bitmap, uint32_t index) {
    bitmap[index / 8] |= (uint8_t)(1U << (index % 8));
}

static uint32_t find_free_inode(const uint8_t *inode_bitmap, uint32_t inode_count) {
    for (uint32_t i = 0; i < inode_count; ++i) {
        if (!bitmap_test(inode_bitmap, i)) {
            return i;
        }
    }
    return (uint32_t)-1;
}

static int find_free_dirent_slot(const struct dirent *dirents, uint32_t max_entries) {
    for (uint32_t i = 0; i < max_entries; ++i) {
        // A free slot has inode == 0 AND empty name (first byte is '\0')
        if (dirents[i].inode == 0 && dirents[i].name[0] == '\0') {
            return (int)i;
        }
    }
    return -1;
}

static void read_journal(int fd, uint8_t *journal_buf) {
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; ++i) {
        read_block(fd, JOURNAL_BLOCK_IDX + i, journal_buf + (i * BLOCK_SIZE));
    }
}

static void write_journal(int fd, const uint8_t *journal_buf) {
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; ++i) {
        write_block(fd, JOURNAL_BLOCK_IDX + i, journal_buf + (i * BLOCK_SIZE));
    }
}

static int cmd_create(int fd, const char *filename) {
    // Read superblock
    struct superblock sb;
    read_block(fd, 0, &sb);
    
    if (sb.magic != FS_MAGIC) {
        fprintf(stderr, "Invalid filesystem magic\n");
        return 1;
    }

    // Read journal
    size_t journal_size = JOURNAL_BLOCKS * BLOCK_SIZE;
    uint8_t *journal_buf = malloc(journal_size);
    if (!journal_buf) {
        die("malloc journal_buf");
    }
    read_journal(fd, journal_buf);

    // Initialize journal if needed
    struct journal_header *jh = (struct journal_header *)journal_buf;
    if (jh->magic != JOURNAL_MAGIC) {
        jh->magic = JOURNAL_MAGIC;
        jh->nbytes_used = sizeof(struct journal_header);
    }

    // Check available space (we need 4 DATA records + 1 COMMIT)
    uint32_t available = journal_size - jh->nbytes_used;
    uint32_t required = 4 * sizeof(struct data_record) + sizeof(struct rec_header);
    if (available < required) {
        fprintf(stderr, "Journal full. Run 'install' first.\n");
        free(journal_buf);
        return 1;
    }

    // Read current metadata
    uint8_t inode_bitmap[BLOCK_SIZE];
    uint8_t data_bitmap[BLOCK_SIZE];
    read_block(fd, INODE_BMAP_IDX, inode_bitmap);
    read_block(fd, DATA_BMAP_IDX, data_bitmap);

    // Read inode table
    size_t inode_table_size = INODE_BLOCKS * BLOCK_SIZE;
    uint8_t *inode_table = malloc(inode_table_size);
    if (!inode_table) {
        die("malloc inode_table");
    }
    for (uint32_t i = 0; i < INODE_BLOCKS; ++i) {
        read_block(fd, INODE_START_IDX + i, inode_table + (i * BLOCK_SIZE));
    }

    // Read root directory (inode 0)
    struct inode *inodes = (struct inode *)inode_table;
    struct inode *root = &inodes[0];
    
    if (root->type != 2) {
        fprintf(stderr, "Root is not a directory\n");
        free(journal_buf);
        free(inode_table);
        return 1;
    }

    // Read root directory data block
    uint8_t root_dir_block[BLOCK_SIZE];
    read_block(fd, root->direct[0], root_dir_block);
    struct dirent *root_dirents = (struct dirent *)root_dir_block;

    // Find free inode
    uint32_t free_inode = find_free_inode(inode_bitmap, sb.inode_count);
    if (free_inode == (uint32_t)-1) {
        fprintf(stderr, "No free inodes available\n");
        free(journal_buf);
        free(inode_table);
        return 1;
    }

    // Find free directory entry slot
    uint32_t max_entries = BLOCK_SIZE / sizeof(struct dirent);
    int free_slot = find_free_dirent_slot(root_dirents, max_entries);
    if (free_slot < 0) {
        fprintf(stderr, "No free directory entry slots\n");
        free(journal_buf);
        free(inode_table);
        return 1;
    }

    // Compute new metadata blocks in memory
    uint8_t new_inode_bitmap[BLOCK_SIZE];
    uint8_t new_root_dir_block[BLOCK_SIZE];
    
    memcpy(new_inode_bitmap, inode_bitmap, BLOCK_SIZE);
    bitmap_set(new_inode_bitmap, free_inode);

    // Determine which inode block contains the new inode
    uint32_t inode_block_idx = free_inode / (BLOCK_SIZE / INODE_SIZE);
    uint32_t inode_offset_in_block = (free_inode % (BLOCK_SIZE / INODE_SIZE)) * INODE_SIZE;
    
    // Determine which inode block contains root (inode 0)
    uint32_t root_inode_block_idx = 0;
    
    // Update root directory
    memcpy(new_root_dir_block, root_dir_block, BLOCK_SIZE);
    struct dirent *new_dirents = (struct dirent *)new_root_dir_block;
    new_dirents[free_slot].inode = free_inode;
    strncpy(new_dirents[free_slot].name, filename, sizeof(new_dirents[free_slot].name) - 1);
    new_dirents[free_slot].name[sizeof(new_dirents[free_slot].name) - 1] = '\0';

    // Update root inode size
    struct inode new_root = *root;
    new_root.size += sizeof(struct dirent);
    time_t now = time(NULL);
    new_root.mtime = (uint32_t)now;

    // Read and update inode blocks
    // If root and new inode are in the same block, handle them together
    uint8_t inode_block_buf[BLOCK_SIZE];
    if (inode_block_idx == root_inode_block_idx) {
        // Both inodes in same block
        read_block(fd, INODE_START_IDX + inode_block_idx, inode_block_buf);
        // Update root inode (at offset 0)
        memcpy(inode_block_buf, &new_root, sizeof(struct inode));
        // Create new file inode
        struct inode *new_inode = (struct inode *)(inode_block_buf + inode_offset_in_block);
        memset(new_inode, 0, sizeof(struct inode));
        new_inode->type = 1; // file
        new_inode->links = 1;
        new_inode->size = 0;
        new_inode->ctime = (uint32_t)now;
        new_inode->mtime = (uint32_t)now;
    } else {
        // Different blocks - read new inode block
        read_block(fd, INODE_START_IDX + inode_block_idx, inode_block_buf);
        // Create new file inode
        struct inode *new_inode = (struct inode *)(inode_block_buf + inode_offset_in_block);
        memset(new_inode, 0, sizeof(struct inode));
        new_inode->type = 1; // file
        new_inode->links = 1;
        new_inode->size = 0;
        new_inode->ctime = (uint32_t)now;
        new_inode->mtime = (uint32_t)now;
    }

    // Read root inode block (if different from new inode block)
    uint8_t root_inode_block_buf[BLOCK_SIZE];
    if (inode_block_idx != root_inode_block_idx) {
        read_block(fd, INODE_START_IDX + root_inode_block_idx, root_inode_block_buf);
        memcpy(root_inode_block_buf, &new_root, sizeof(struct inode));
    }

    // Append DATA records to journal
    uint32_t offset = jh->nbytes_used;
    int data_records_count = 0;

    // Log inode bitmap
    struct data_record *rec = (struct data_record *)(journal_buf + offset);
    rec->hdr.type = REC_DATA;
    rec->hdr.size = sizeof(struct data_record);
    rec->block_no = INODE_BMAP_IDX;
    memcpy(rec->data, new_inode_bitmap, BLOCK_SIZE);
    offset += sizeof(struct data_record);
    data_records_count++;

    // Log inode table blocks
    if (inode_block_idx == root_inode_block_idx) {
        // Single block contains both inodes
        rec = (struct data_record *)(journal_buf + offset);
        rec->hdr.type = REC_DATA;
        rec->hdr.size = sizeof(struct data_record);
        rec->block_no = INODE_START_IDX + inode_block_idx;
        memcpy(rec->data, inode_block_buf, BLOCK_SIZE);
        offset += sizeof(struct data_record);
        data_records_count++;
    } else {
        // Log new inode block
        rec = (struct data_record *)(journal_buf + offset);
        rec->hdr.type = REC_DATA;
        rec->hdr.size = sizeof(struct data_record);
        rec->block_no = INODE_START_IDX + inode_block_idx;
        memcpy(rec->data, inode_block_buf, BLOCK_SIZE);
        offset += sizeof(struct data_record);
        data_records_count++;
        
        // Log root inode block
        rec = (struct data_record *)(journal_buf + offset);
        rec->hdr.type = REC_DATA;
        rec->hdr.size = sizeof(struct data_record);
        rec->block_no = INODE_START_IDX + root_inode_block_idx;
        memcpy(rec->data, root_inode_block_buf, BLOCK_SIZE);
        offset += sizeof(struct data_record);
        data_records_count++;
    }

    // Log root directory block
    rec = (struct data_record *)(journal_buf + offset);
    rec->hdr.type = REC_DATA;
    rec->hdr.size = sizeof(struct data_record);
    rec->block_no = root->direct[0];
    memcpy(rec->data, new_root_dir_block, BLOCK_SIZE);
    offset += sizeof(struct data_record);

    // Append COMMIT record
    struct rec_header *commit = (struct rec_header *)(journal_buf + offset);
    commit->type = REC_COMMIT;
    commit->size = sizeof(struct rec_header);
    offset += sizeof(struct rec_header);

    // Update journal header
    jh->nbytes_used = offset;

    // Write journal back
    write_journal(fd, journal_buf);

    free(journal_buf);
    free(inode_table);
    return 0;
}

static int cmd_install(int fd) {
    // Read journal
    size_t journal_size = JOURNAL_BLOCKS * BLOCK_SIZE;
    uint8_t *journal_buf = malloc(journal_size);
    if (!journal_buf) {
        die("malloc journal_buf");
    }
    read_journal(fd, journal_buf);

    // Check journal header
    struct journal_header *jh = (struct journal_header *)journal_buf;
    if (jh->magic != JOURNAL_MAGIC) {
        fprintf(stderr, "Journal does not exist\n");
        free(journal_buf);
        return 1;
    }

    if (jh->nbytes_used <= sizeof(struct journal_header)) {
        // Journal is empty
        free(journal_buf);
        return 0;
    }

    
    uint32_t offset = sizeof(struct journal_header);
    
    while (offset < jh->nbytes_used) {
       
        uint32_t transaction_start = offset;
        int found_commit = 0;
        
    
        uint32_t scan_offset = offset;
        while (scan_offset < jh->nbytes_used) {
            
            if (scan_offset + sizeof(struct rec_header) > jh->nbytes_used) {
                break; 
            }

            struct rec_header *hdr = (struct rec_header *)(journal_buf + scan_offset);
            
            
            if (scan_offset + hdr->size > jh->nbytes_used) {
                break;
            }

            if (hdr->type == REC_COMMIT) {
                found_commit = 1;
                scan_offset += hdr->size;
                break;
            } else if (hdr->type == REC_DATA) {
                scan_offset += hdr->size;
            } else {
                
                scan_offset += hdr->size;
            }
        }

        if (found_commit) {
         
            uint32_t replay_offset = transaction_start;
            while (replay_offset < scan_offset) {
                struct rec_header *hdr = (struct rec_header *)(journal_buf + replay_offset);
                if (hdr->type == REC_DATA) {
                    struct data_record *rec = (struct data_record *)(journal_buf + replay_offset);
                    
                    write_block(fd, rec->block_no, rec->data);
                }
                replay_offset += hdr->size;
            }
            offset = scan_offset; 
        } else {
            
            break;
        }
    }

    
    memset(journal_buf, 0, journal_size);
    jh->magic = JOURNAL_MAGIC;
    jh->nbytes_used = sizeof(struct journal_header);
    write_journal(fd, journal_buf);

    free(journal_buf);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <create <name>|install>\n", argv[0]);
        return 1;
    }

    const char *image_path = DEFAULT_IMAGE;
    int fd = open(image_path, O_RDWR);
    if (fd < 0) {
        die("open");
    }

    int ret = 0;
    if (strcmp(argv[1], "create") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s create <name>\n", argv[0]);
            ret = 1;
        } else {
            ret = cmd_create(fd, argv[2]);
        }
    } else if (strcmp(argv[1], "install") == 0) {
        ret = cmd_install(fd);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        ret = 1;
    }

    if (close(fd) < 0) {
        die("close");
    }

    return ret;
}
