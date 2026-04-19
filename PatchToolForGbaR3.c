/*
 * PatchToolForGbaR3 - Custom GBA ROM Patch Tool
 * 
 * Compile with: gcc PatchToolForGbaR3.c -o PatchToolForGbaR3.exe -O2 -std=c99 -Wall
 * 
 * This tool implements the custom GR3 patch format for GBA ROMs,
 * supporting patch creation, applying, conversion between patch, json and ips formats.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/* structure of the custom patch file format (GR3 Patch):
// [Header]  little endian
    char     magic[8];							// fixed "PATCHGR3"
    uint32_t filesize;							// whole sizes of patch file
    uint32_t gamecode;						    // gamecode of gba like "BPEE"
    uint8_t  version;							// reversion of gba like [0x00]
    uint8_t  pad[3];							// [0x00, 0x00, 0x00]
    uint32_t rom_block_index_address;		    // rom block index address in thie patch file (0 means no rom patch)
    uint32_t rom_block_count;				    // counts of rom blocks need to be patched
    uint32_t rom_block_index_size;			    // wholse sizes of rom block index
    uint16_t rom_block_min;					    // the min id of rom block need to be patched (0-8191)
    uint16_t rom_block_max;					    // the max id of rom block need to be patched (0-8191)

// [Rom Block Index] align 16 little endian
{
    uint32_t block_patch_address;				// block patch address in thie patch file
    uint32_t block_patch_count;					// counts of each patch data (see how ips contains patch data)
    uint32_t block_patch_size;					// whole sizes of this block patch
    uint32_t block_index;						// index number of this block (0-8191)
} base structe of index, then add any blocks need to be patched in this struct


// [Rom block Patch] align 16
The .ips data for each rom blocks of gba rom listed in rom block Index. each block has its own ips data.
The offset address used is a relative address within this rom block.
{
"PTACH" + data + "EOF"
data-Standard Record:
    Offset：3-byte big-endian address
    Size：2-byte non-zero big-endian length
    Data：variable-length payload bytes
data-RLE Record:
    Offset：3-byte big-endian address
    Size：2-byte 0x0000 (RLE flag)
    RLE Length：2-byte repeat count
    RLE Byte：1-byte value to repeat
if there are >=8 repeated changed bytes need to be store, use rle record. if <8 repeated changed bytes, use standard record.
}

[Foot] align 16
    char     magic[6];							// fixed "EOFGR3"
*/

// Constants
#define HEADER_MAGIC "PATCHGR3"
#define FOOTER_MAGIC "EOFGR3"
#define IPS_MAGIC "PATCH"
#define IPS_EOF "EOF"
#define BLOCK_SIZE (4 * 1024)                   // 4KB per rom block
#define MAX_ROM_BLOCK 8191                      // Max rom block ID (32MB total)
#define MAX_IPS_SIZE (16 * 1024 * 1024)         // IPS max supported size 16MB
#define GAMECODE_OFFSET 0xAC                    // Gamecode offset in ROM for default filename
#define VERSION_OFFSET 0xBC                     // Version offset in ROM for default filename
// IPS Record Structure
typedef struct {
    uint32_t offset;
    uint16_t size; // 0 means RLE record
    union {
        struct {
            uint16_t rle_len;
            uint8_t rle_byte;
        } rle;
        struct {
            uint8_t *data;
        } std;
    } d;
} IPSRecord;
// Rom Block Structure
typedef struct {
    uint32_t block_id;
    IPSRecord *records;
    uint32_t record_count;
    uint32_t record_capacity;
    uint8_t *ips_data;
    uint32_t ips_size;
} RomBlock;
// Compare function for qsort
static int compare_ips_records(const void *a, const void *b) {
    const IPSRecord *ra = (const IPSRecord*)a;
    const IPSRecord *rb = (const IPSRecord*)b;
    if (ra->offset > rb->offset) return 1;
    if (ra->offset < rb->offset) return -1;
    return 0;
}
// Dynamic array helpers
#define INIT_CAPACITY 16
static void add_record_to_array(IPSRecord **records, uint32_t *count, uint32_t *capacity, IPSRecord record) {
    if (*count >= *capacity) {
        *capacity = (*capacity == 0) ? INIT_CAPACITY : (*capacity * 2);
        *records = realloc(*records, *capacity * sizeof(IPSRecord));
    }
    (*records)[(*count)++] = record;
}
static void add_block_to_array(RomBlock **blocks, uint32_t *count, uint32_t *capacity, RomBlock block) {
    if (*count >= *capacity) {
        *capacity = (*capacity == 0) ? INIT_CAPACITY : (*capacity * 2);
        *blocks = realloc(*blocks, *capacity * sizeof(RomBlock));
    }
    (*blocks)[(*count)++] = block;
}
// Byte order helper functions
static uint16_t read_be16(const uint8_t *buf) {
    return (buf[0] << 8) | buf[1];
}
static uint32_t read_be24(const uint8_t *buf) {
    return (buf[0] << 16) | (buf[1] << 8) | buf[2];
}
static uint32_t read_le32(const uint8_t *buf) {
    return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}
static uint16_t read_le16(const uint8_t *buf) {
    return buf[0] | (buf[1] << 8);
}
static void write_be16(uint8_t *buf, uint16_t v) {
    buf[0] = (v >> 8) & 0xFF;
    buf[1] = v & 0xFF;
}
static void write_be24(uint8_t *buf, uint32_t v) {
    buf[0] = (v >> 16) & 0xFF;
    buf[1] = (v >> 8) & 0xFF;
    buf[2] = v & 0xFF;
}
static void write_le32(uint8_t *buf, uint32_t v) {
    buf[0] = v & 0xFF;
    buf[1] = (v >> 8) & 0xFF;
    buf[2] = (v >> 16) & 0xFF;
    buf[3] = (v >> 24) & 0xFF;
}
static void write_le16(uint8_t *buf, uint16_t v) {
    buf[0] = v & 0xFF;
    buf[1] = (v >> 8) & 0xFF;
}
// Align to 16 bytes helper
static uint32_t align16(uint32_t addr) {
    return (addr + 15) & ~15;
}
// Parse IPS data into records
static int parse_ips_data(uint8_t *ips_data, uint32_t ips_size, IPSRecord **out_records, uint32_t *out_count) {
    if (ips_size < 8 || memcmp(ips_data, IPS_MAGIC, 5) != 0) {
        return -1; // Invalid IPS header
    }
    IPSRecord *records = NULL;
    uint32_t count = 0, capacity = 0;
    uint32_t pos = 5;
    while (pos < ips_size - 3) {
        if (memcmp(&ips_data[pos], IPS_EOF, 3) == 0) break;
        if (pos + 5 > ips_size) break;
        uint32_t offset = read_be24(&ips_data[pos]);
        pos +=3;
        uint16_t size = read_be16(&ips_data[pos]);
        pos +=2;
        IPSRecord rec;
        rec.offset = offset;
        rec.size = size;
        if (size == 0) {
            // RLE record
            if (pos +3 > ips_size) break;
            rec.d.rle.rle_len = read_be16(&ips_data[pos]);
            pos +=2;
            rec.d.rle.rle_byte = ips_data[pos];
            pos +=1;
        } else {
            // Standard record
            if (pos + size > ips_size) break;
            rec.d.std.data = malloc(size);
            memcpy(rec.d.std.data, &ips_data[pos], size);
            pos += size;
        }
        add_record_to_array(&records, &count, &capacity, rec);
    }
    *out_records = records;
    *out_count = count;
    return 0;
}
// Generate IPS data from records
static int generate_ips_data(IPSRecord *records, uint32_t count, uint8_t **out_data, uint32_t *out_size) {
    // Calculate required size first
    uint32_t size = 5 + 3; // PATCH + EOF
    for (uint32_t i=0; i<count; i++) {
        size += 5; // offset + size
        if (records[i].size == 0) {
            size +=3; // RLE len + byte
        } else {
            size += records[i].size;
        }
    }
    uint8_t *data = malloc(size);
    uint32_t pos =0;
    memcpy(data, IPS_MAGIC,5);
    pos +=5;
    for (uint32_t i=0; i<count; i++) {
        write_be24(&data[pos], records[i].offset);
        pos +=3;
        write_be16(&data[pos], records[i].size);
        pos +=2;
        if (records[i].size ==0) {
            write_be16(&data[pos], records[i].d.rle.rle_len);
            pos +=2;
            data[pos] = records[i].d.rle.rle_byte;
            pos +=1;
        } else {
            memcpy(&data[pos], records[i].d.std.data, records[i].size);
            pos += records[i].size;
        }
    }
    memcpy(&data[pos], IPS_EOF,3);
    pos +=3;
    *out_data = data;
    *out_size = pos;
    return 0;
}
// Process diff segment into records
static void process_diff_segment(uint32_t abs_addr, uint8_t *data, uint32_t len, 
                                RomBlock **blocks, uint32_t *block_count, uint32_t *block_cap) {
    uint32_t i=0;
    while (i < len) {
        uint8_t current_byte = data[i];
        uint32_t run_len =1;
        while (i+run_len < len && data[i+run_len] == current_byte) {
            run_len++;
        }
        if (run_len >= 8) {
            // RLE record, split if crosses 4KB block boundary
            uint32_t remaining = run_len;
            uint32_t current_addr = abs_addr +i;
            while (remaining > 0) {
                uint32_t block_id = current_addr / BLOCK_SIZE;
                uint32_t block_offset = current_addr % BLOCK_SIZE;
                // Truncate chunk to current block boundary
                uint32_t chunk_len = remaining;
                if (block_offset + chunk_len > BLOCK_SIZE) {
                    chunk_len = BLOCK_SIZE - block_offset;
                }
                
                IPSRecord rec;
                rec.offset = block_offset;
                rec.size =0;
                rec.d.rle.rle_len = chunk_len;
                rec.d.rle.rle_byte = current_byte;
                
                // Find or create target block
                RomBlock *block = NULL;
                for (uint32_t j=0; j<*block_count; j++) {
                    if ((*blocks)[j].block_id == block_id) {
                        block = &(*blocks)[j];
                        break;
                    }
                }
                if (!block) {
                    RomBlock new_block = {0};
                    new_block.block_id = block_id;
                    add_block_to_array(blocks, block_count, block_cap, new_block);
                    block = &(*blocks)[*block_count -1];
                }
                add_record_to_array(&block->records, &block->record_count, &block->record_capacity, rec);
                
                current_addr += chunk_len;
                remaining -= chunk_len;
            }
            i += run_len;
        } else {
            // Standard record, collect until next RLE, split if crosses block boundary
            uint32_t std_start = i;
            while (i < len) {
                uint8_t b = data[i];
                uint32_t r=1;
                while (i+r < len && data[i+r] == b) r++;
                if (r >= 8) break;
                i++;
            }
            uint32_t remaining = i - std_start;
            uint32_t current_addr = abs_addr + std_start;
            uint32_t data_offset = std_start;
            while (remaining > 0) {
                uint32_t block_id = current_addr / BLOCK_SIZE;
                uint32_t block_offset = current_addr % BLOCK_SIZE;
                // Truncate chunk to current block boundary
                uint32_t chunk_len = remaining;
                if (block_offset + chunk_len > BLOCK_SIZE) {
                    chunk_len = BLOCK_SIZE - block_offset;
                }
                
                IPSRecord rec;
                rec.offset = block_offset;
                rec.size = chunk_len;
                rec.d.std.data = malloc(chunk_len);
                memcpy(rec.d.std.data, &data[data_offset], chunk_len);
                
                // Find or create target block
                RomBlock *block = NULL;
                for (uint32_t j=0; j<*block_count; j++) {
                    if ((*blocks)[j].block_id == block_id) {
                        block = &(*blocks)[j];
                        break;
                    }
                }
                if (!block) {
                    RomBlock new_block = {0};
                    new_block.block_id = block_id;
                    add_block_to_array(blocks, block_count, block_cap, new_block);
                    block = &(*blocks)[*block_count -1];
                }
                add_record_to_array(&block->records, &block->record_count, &block->record_capacity, rec);
                
                current_addr += chunk_len;
                data_offset += chunk_len;
                remaining -= chunk_len;
            }
        }
    }
}
// Create patch mode
static int mode_create(const char *orig_path, const char *new_path, const char *out_patch_path) {
    printf("Creating patch from %s to %s...\n", orig_path, new_path);
    // Read original file
    FILE *f_orig = fopen(orig_path, "rb");
    if (!f_orig) {
        perror("Cannot open original file");
        return -1;
    }
    fseek(f_orig, 0, SEEK_END);
    long orig_size = ftell(f_orig);
    fseek(f_orig, 0, SEEK_SET);
    uint8_t *orig_buf = malloc(orig_size);
    fread(orig_buf, 1, orig_size, f_orig);
    fclose(f_orig);
    // Read new file
    FILE *f_new = fopen(new_path, "rb");
    if (!f_new) {
        perror("Cannot open new file");
        free(orig_buf);
        return -1;
    }
    fseek(f_new, 0, SEEK_END);
    long new_size = ftell(f_new);
    fseek(f_new, 0, SEEK_SET);
    uint8_t *new_buf = malloc(new_size);
    fread(new_buf, 1, new_size, f_new);
    fclose(f_new);
    // Process diffs
    RomBlock *blocks = NULL;
    uint32_t block_count=0, block_cap=0;
    long max_size = orig_size > new_size ? orig_size : new_size;
    
    long i=0;
    while (i < max_size) {
        while (i < max_size) {
            uint8_t orig_b = (i < orig_size) ? orig_buf[i] : 0xFF;
            uint8_t new_b = (i < new_size) ? new_buf[i] : 0xFF;
            if (orig_b != new_b) break;
            i++;
        }
        if (i >= max_size) break;
        
        long start = i;
        int consecutive_unchanged = 0;
        
        while (i < max_size) {
            uint8_t orig_b = (i < orig_size) ? orig_buf[i] : 0xFF;
            uint8_t new_b = (i < new_size) ? new_buf[i] : 0xFF;
            
            if (orig_b == new_b) {
                consecutive_unchanged++;
                if (consecutive_unchanged >= 6) {
                    break;
                }
            } else {
                consecutive_unchanged = 0;
            }
            i++;
        }
        
        long end = i;
        while (end > start) {
            long pos = end -1;
            uint8_t orig_b = (pos < orig_size) ? orig_buf[pos] : 0xFF;
            uint8_t new_b = (pos < new_size) ? new_buf[pos] : 0xFF;
            if (orig_b == new_b) {
                end--;
            } else {
                break;
            }
        }
        
        long len = end - start;
        if (len <=0) continue;
        
        uint8_t *diff_data = malloc(len);
        for (long j=0; j<len; j++) {
            diff_data[j] = new_buf[start +j];
        }
        process_diff_segment(start, diff_data, len, 
                           &blocks, &block_count, &block_cap);
        free(diff_data);
    }
    // Generate IPS data
    for (uint32_t i=0; i<block_count; i++) {
        if (blocks[i].record_count >0) {
            generate_ips_data(blocks[i].records, blocks[i].record_count, 
                             &blocks[i].ips_data, &blocks[i].ips_size);
        }
    }
    // Determine output filename
    char patch_path[256], json_path[256];
    if (out_patch_path) {
        // User specified path
        strncpy(patch_path, out_patch_path, sizeof(patch_path)-1);
        // Check if has .patch suffix
        const char *ext = strrchr(patch_path, '.');
        if (!ext || strcmp(ext, ".patch") !=0) {
            strcat(patch_path, ".patch");
        }
    } else {
        // Default filename from gamecode and version
        char gamecode[5] = "UNKN";
        uint8_t version =0;
        if (orig_size >= VERSION_OFFSET +1) {
            memcpy(gamecode, &orig_buf[GAMECODE_OFFSET],4);
            gamecode[4] =0;
            version = orig_buf[VERSION_OFFSET];
        }
        snprintf(patch_path, sizeof(patch_path), "%s%02X.patch", gamecode, version);
    }
    // Json path
    snprintf(json_path, sizeof(json_path), "%s.json", patch_path);
    // Build patch file
    // Calculate positions
    uint32_t header_size = 36; // Fixed new header size
    uint32_t current_pos = header_size;
    uint32_t index_addr =0;
    uint32_t index_size = block_count * 16;
    if (block_count >0) {
        index_addr = current_pos;
        current_pos = align16(current_pos + index_size);
    }
    // Block positions
    uint32_t *block_addrs = malloc(block_count * sizeof(uint32_t));
    for (uint32_t i=0; i<block_count; i++) {
        block_addrs[i] = current_pos;
        current_pos = align16(current_pos + blocks[i].ips_size);
    }
    // Footer position
    uint32_t footer_pos = align16(current_pos);
    uint32_t total_size = footer_pos +6;
    // Write patch file
    FILE *f_patch = fopen(patch_path, "wb");
    if (!f_patch) {
        perror("Cannot create patch file");
        goto cleanup;
    }
    // Write header
    uint8_t header[36] = {0};
    memcpy(header, HEADER_MAGIC,8);
    write_le32(&header[8], total_size);
    // Gamecode and version from original file
    if (orig_size >= VERSION_OFFSET +1) {
        memcpy(&header[12], &orig_buf[GAMECODE_OFFSET],4);
        header[16] = orig_buf[VERSION_OFFSET];
    }
    // Pad is already 0
    write_le32(&header[20], index_addr);
    write_le32(&header[24], block_count);
    write_le32(&header[28], index_size);
    // Min and max block id
    uint16_t min_block = 0, max_block =0;
    if (block_count >0) {
        min_block = blocks[0].block_id;
        max_block = blocks[0].block_id;
        for (uint32_t i=1; i<block_count; i++) {
            if (blocks[i].block_id < min_block) min_block = blocks[i].block_id;
            if (blocks[i].block_id > max_block) max_block = blocks[i].block_id;
        }
    }
    write_le16(&header[32], min_block);
    write_le16(&header[34], max_block);
    fwrite(header, 1, 36, f_patch);
    // Write index
    if (block_count >0) {
        for (uint32_t i=0; i<block_count; i++) {
            uint8_t idx[16];
            write_le32(&idx[0], block_addrs[i]);
            write_le32(&idx[4], blocks[i].record_count);
            write_le32(&idx[8], blocks[i].ips_size);
            write_le32(&idx[12], blocks[i].block_id);
            fwrite(idx, 1,16, f_patch);
        }
        // Padding
        uint32_t pad = align16(index_addr + index_size) - (index_addr + index_size);
        uint8_t zero[16] = {0};
        fwrite(zero, 1, pad, f_patch);
    }
    // Write block patches
    for (uint32_t i=0; i<block_count; i++) {
        fwrite(blocks[i].ips_data, 1, blocks[i].ips_size, f_patch);
        uint32_t pad = align16(block_addrs[i] + blocks[i].ips_size) - (block_addrs[i] + blocks[i].ips_size);
        uint8_t zero[16] = {0};
        fwrite(zero, 1, pad, f_patch);
    }
    // Write footer
    uint32_t pad = footer_pos - current_pos;
    uint8_t zero[16] = {0};
    fwrite(zero, 1, pad, f_patch);
    fwrite(FOOTER_MAGIC, 1,6, f_patch);
    fclose(f_patch);
    printf("Patch file created: %s\n", patch_path);
    // Now generate json file
    FILE *f_json = fopen(json_path, "w");
    if (f_json) {
        fprintf(f_json, "{\n");
        fprintf(f_json, "  \"header\": {\n");
        fprintf(f_json, "    \"magic\": \"%s\",\n", HEADER_MAGIC);
        fprintf(f_json, "    \"filesize\": \"0x%08X\",\n", total_size);
        fprintf(f_json, "    \"gamecode\": \"%.4s\",\n", &header[12]);
        fprintf(f_json, "    \"version\": \"0x%02X\",\n", header[16]);
        fprintf(f_json, "    \"rom_block_index_address\": \"0x%08X\",\n", index_addr);
        fprintf(f_json, "    \"rom_block_count\": %u,\n", block_count);
        fprintf(f_json, "    \"rom_block_index_size\": \"0x%08X\",\n", index_size);
        fprintf(f_json, "    \"rom_block_min\": 0x%04X,\n", min_block);
        fprintf(f_json, "    \"rom_block_max\": 0x%04X\n", max_block);
        fprintf(f_json, "  },\n");
        // Rom blocks
        fprintf(f_json, "  \"rom_blocks\": [\n");
        for (uint32_t i=0; i<block_count; i++) {
            RomBlock *b = &blocks[i];
            fprintf(f_json, "    {\n");
            fprintf(f_json, "      \"block_index\": 0x%04X,\n", b->block_id);
            fprintf(f_json, "      \"block_patch_address\": \"0x%08X\",\n", block_addrs[i]);
            fprintf(f_json, "      \"block_patch_count\": %u,\n", b->record_count);
            fprintf(f_json, "      \"block_patch_size\": \"0x%08X\",\n", b->ips_size);
            fprintf(f_json, "      \"records\": [\n");
            for (uint32_t j=0; j<b->record_count; j++) {
                IPSRecord *r = &b->records[j];
                fprintf(f_json, "        { \"offset\": \"0x%06X\"", r->offset);
                if (r->size ==0) {
                    fprintf(f_json, ", \"rle_length\": \"0x%04X\", \"rle_byte\": \"0x%02X\"", r->d.rle.rle_len, r->d.rle.rle_byte);
                } else {
                    fprintf(f_json, ", \"size\": \"0x%04X\", \"data\": [", r->size);
                    for (uint32_t k=0; k<r->size; k++) {
                        if (k>0) fprintf(f_json, ", ");
                        fprintf(f_json, "\"%02X\"", r->d.std.data[k]);
                    }
                    fprintf(f_json, "]");
                }
                if (j < b->record_count -1) fprintf(f_json, " },\n");
                else fprintf(f_json, " }\n");
            }
            fprintf(f_json, "      ]\n");
            if (i < block_count -1) fprintf(f_json, "    },\n");
            else fprintf(f_json, "    }\n");
        }
        fprintf(f_json, "  ],\n");
        fprintf(f_json, "  \"footer\": {\n");
        fprintf(f_json, "    \"magic\": \"%s\"\n", FOOTER_MAGIC);
        fprintf(f_json, "  }\n");
        fprintf(f_json, "}\n");
        fclose(f_json);
        printf("JSON file created: %s\n", json_path);
    }
cleanup:
    // Cleanup
    free(orig_buf);
    free(new_buf);
    for (uint32_t i=0; i<block_count; i++) {
        for (uint32_t j=0; j<blocks[i].record_count; j++) {
            if (blocks[i].records[j].size !=0) free(blocks[i].records[j].d.std.data);
        }
        free(blocks[i].records);
        free(blocks[i].ips_data);
    }
    free(blocks);
    free(block_addrs);
    return 0;
}
// Apply patch mode
static int mode_apply(const char *patch_path, const char *orig_path, const char *out_rom_path) {
    printf("Applying patch %s to %s...\n", patch_path, orig_path);
    // Check patch suffix
    const char *ext = strrchr(patch_path, '.');
    if (!ext || strcmp(ext, ".patch") !=0) {
        fprintf(stderr, "Warning: Patch file does not have .patch suffix\n");
    }
    // Determine output filename
    char out_path[256];
    if (out_rom_path) {
        strncpy(out_path, out_rom_path, sizeof(out_path)-1);
    } else {
        // Add _patched suffix
        strncpy(out_path, orig_path, sizeof(out_path)-1);
        char *ext_p = strrchr(out_path, '.');
        if (ext_p) {
            snprintf(ext_p, sizeof(out_path) - (ext_p - out_path), "_patched%s", ext_p);
        } else {
            strcat(out_path, "_patched");
        }
    }
    // Read patch file
    FILE *f_patch = fopen(patch_path, "rb");
    if (!f_patch) {
        perror("Cannot open patch file");
        return -1;
    }
    fseek(f_patch, 0, SEEK_END);
    long patch_size = ftell(f_patch);
    fseek(f_patch, 0, SEEK_SET);
    uint8_t *patch_buf = malloc(patch_size);
    fread(patch_buf, 1, patch_size, f_patch);
    fclose(f_patch);
    // Verify header
    if (memcmp(patch_buf, HEADER_MAGIC,8) !=0) {
        fprintf(stderr, "Invalid patch file: Wrong header magic\n");
        free(patch_buf);
        return -1;
    }
    uint32_t filesize = read_le32(&patch_buf[8]);
    if (filesize != patch_size) {
        fprintf(stderr, "Warning: Patch file size mismatch, header says 0x%X, actual 0x%lX\n", filesize, patch_size);
    }
    uint8_t gamecode[5];
    memcpy(gamecode, &patch_buf[12],4);
    gamecode[4] =0;
    uint8_t version = patch_buf[16];
    uint32_t index_addr = read_le32(&patch_buf[20]);
    uint32_t block_count = read_le32(&patch_buf[24]);
    uint32_t index_size = read_le32(&patch_buf[28]);
    // Verify footer
    if (memcmp(&patch_buf[patch_size -6], FOOTER_MAGIC,6) !=0) {
        fprintf(stderr, "Invalid patch file: Wrong footer magic\n");
        free(patch_buf);
        return -1;
    }
    // Read original file
    FILE *f_orig = fopen(orig_path, "rb");
    if (!f_orig) {
        perror("Cannot open original file");
        free(patch_buf);
        return -1;
    }
    fseek(f_orig, 0, SEEK_END);
    long orig_size = ftell(f_orig);
    fseek(f_orig, 0, SEEK_SET);
    // Calculate new size, find max address from patches
    long new_size = orig_size;
    // Check block patches
    for (uint32_t i=0; i<block_count; i++) {
        uint8_t *idx = &patch_buf[index_addr + i*16];
        uint32_t b_addr = read_le32(&idx[0]);
        uint32_t b_cnt = read_le32(&idx[4]);
        uint32_t b_size = read_le32(&idx[8]);
        uint32_t b_id = read_le32(&idx[12]);
        uint32_t b_base = b_id * BLOCK_SIZE;
        IPSRecord *recs;
        uint32_t cnt;
        parse_ips_data(&patch_buf[b_addr], b_size, &recs, &cnt);
        for (uint32_t j=0; j<cnt; j++) {
            long max_addr;
            if (recs[j].size ==0) {
                max_addr = b_base + recs[j].offset + recs[j].d.rle.rle_len;
            } else {
                max_addr = b_base + recs[j].offset + recs[j].size;
            }
            if (max_addr > new_size) new_size = max_addr;
        }
        // Cleanup
        for (uint32_t j=0; j<cnt; j++) {
            if (recs[j].size !=0) free(recs[j].d.std.data);
        }
        free(recs);
    }
    // Allocate buffer, fill with 0xFF
    uint8_t *rom_buf = malloc(new_size);
    memset(rom_buf, 0xFF, new_size);
    // Copy original data
    fread(rom_buf, 1, orig_size, f_orig);
    fclose(f_orig);
    // Apply block patches
    for (uint32_t i=0; i<block_count; i++) {
        uint8_t *idx = &patch_buf[index_addr + i*16];
        uint32_t b_addr = read_le32(&idx[0]);
        uint32_t b_size = read_le32(&idx[8]);
        uint32_t b_id = read_le32(&idx[12]);
        uint32_t b_base = b_id * BLOCK_SIZE;
        uint8_t *ips_data = &patch_buf[b_addr];
        if (memcmp(ips_data, IPS_MAGIC,5) ==0) {
            uint32_t pos =5;
            while (pos < b_size -3) {
                if (memcmp(&ips_data[pos], IPS_EOF,3) ==0) break;
                uint32_t offset = read_be24(&ips_data[pos]);
                pos +=3;
                uint16_t size = read_be16(&ips_data[pos]);
                pos +=2;
                uint32_t abs_offset = b_base + offset;
                if (size ==0) {
                    uint16_t rle_len = read_be16(&ips_data[pos]);
                    pos +=2;
                    uint8_t b = ips_data[pos];
                    pos +=1;
                    for (uint32_t j=0; j<rle_len; j++) {
                        if (abs_offset +j < new_size) rom_buf[abs_offset +j] = b;
                    }
                } else {
                    uint32_t copy_size = size;
                    if (abs_offset + copy_size > new_size) copy_size = new_size - abs_offset;
                    if (copy_size >0) {
                        memcpy(&rom_buf[abs_offset], &ips_data[pos], copy_size);
                    }
                    pos += size;
                }
            }
        }
    }
    // Write output
    FILE *f_out = fopen(out_path, "wb");
    if (!f_out) {
        perror("Cannot create output file");
        free(patch_buf);
        free(rom_buf);
        return -1;
    }
    fwrite(rom_buf, 1, new_size, f_out);
    fclose(f_out);
    free(patch_buf);
    free(rom_buf);
    printf("Patched ROM created: %s\n", out_path);
    return 0;
}
// JSON helper functions for parsing
static void skip_whitespace(FILE *f) {
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (!isspace(c)) {
            ungetc(c, f);
            break;
        }
    }
}
static int read_string(FILE *f, char *buf, int buf_size) {
    skip_whitespace(f);
    int c = fgetc(f);
    if (c != '"') return -1;
    int i=0;
    while ((c = fgetc(f)) != EOF) {
        if (c == '"') break;
        if (i < buf_size-1) {
            buf[i++] = c;
        }
    }
    buf[i] =0;
    return 0;
}
static uint32_t read_hex(FILE *f) {
    skip_whitespace(f);
    char buf[20];
    int i=0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (isspace(c) || c == ',' || c == '}' || c == ']') {
            ungetc(c, f);
            break;
        }
        if (i < 19) buf[i++] = c;
    }
    buf[i] =0;
    return strtoul(buf, NULL, 16);
}
static uint32_t read_dec(FILE *f) {
    skip_whitespace(f);
    char buf[20];
    int i=0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (isspace(c) || c == ',' || c == '}' || c == ']') {
            ungetc(c, f);
            break;
        }
        if (i < 19) buf[i++] = c;
    }
    buf[i] =0;
    return strtoul(buf, NULL, 10);
}
static uint8_t *read_data_array(FILE *f, uint32_t *len) {
    skip_whitespace(f);
    int c = fgetc(f);
    if (c != '[') return NULL;
    *len =0;
    uint32_t capacity = 16;
    uint8_t *data = malloc(capacity);
    while (1) {
        skip_whitespace(f);
        c = fgetc(f);
        if (c == ']') break;
        ungetc(c, f);
        char hex[3];
        if (fread(hex, 1, 2, f) !=2) break;
        hex[2] =0;
        uint8_t b = strtoul(hex, NULL, 16);
        if (*len >= capacity) {
            capacity *=2;
            data = realloc(data, capacity);
        }
        data[(*len)++] = b;
        skip_whitespace(f);
        c = fgetc(f);
        if (c == ',') continue;
        else {
            ungetc(c, f);
        }
    }
    return data;
}

// Patch to json mode
static int mode_patch2json(const char *patch_path) {
    printf("Converting patch to JSON: %s...\n", patch_path);
    // Check suffix
    const char *ext = strrchr(patch_path, '.');
    if (!ext || strcmp(ext, ".patch") !=0) {
        fprintf(stderr, "Warning: Patch file does not have .patch suffix\n");
    }
    char json_path[256];
    snprintf(json_path, sizeof(json_path), "%s.json", patch_path);
    // Read patch file
    FILE *f_patch = fopen(patch_path, "rb");
    if (!f_patch) {
        perror("Cannot open patch file");
        return -1;
    }
    fseek(f_patch,0,SEEK_END);
    long patch_size = ftell(f_patch);
    fseek(f_patch,0,SEEK_SET);
    uint8_t *patch_buf = malloc(patch_size);
    fread(patch_buf,1,patch_size,f_patch);
    fclose(f_patch);
    // Verify
    if (memcmp(patch_buf, HEADER_MAGIC,8) !=0) {
        fprintf(stderr, "Invalid patch file\n");
        free(patch_buf);
        return -1;
    }
    uint32_t filesize = read_le32(&patch_buf[8]);
    uint8_t gamecode[5];
    memcpy(gamecode, &patch_buf[12],4);
    gamecode[4] =0;
    uint8_t version = patch_buf[16];
    uint32_t index_addr = read_le32(&patch_buf[20]);
    uint32_t block_count = read_le32(&patch_buf[24]);
    uint32_t index_size = read_le32(&patch_buf[28]);
    uint16_t min_block = read_le16(&patch_buf[32]);
    uint16_t max_block = read_le16(&patch_buf[34]);
    // Parse blocks
    RomBlock *blocks = malloc(block_count * sizeof(RomBlock));
    for (uint32_t i=0; i<block_count; i++) {
        uint8_t *idx = &patch_buf[index_addr + i*16];
        uint32_t b_addr = read_le32(&idx[0]);
        uint32_t b_cnt = read_le32(&idx[4]);
        uint32_t b_size = read_le32(&idx[8]);
        uint32_t b_id = read_le32(&idx[12]);
        blocks[i].block_id = b_id;
        parse_ips_data(&patch_buf[b_addr], b_size, &blocks[i].records, &blocks[i].record_count);
        blocks[i].ips_data = &patch_buf[b_addr];
        blocks[i].ips_size = b_size;
    }
    // Generate json
    FILE *f_json = fopen(json_path, "w");
    if (!f_json) {
        perror("Cannot create json file");
        goto cleanup;
    }
    fprintf(f_json, "{\n");
    fprintf(f_json, "  \"header\": {\n");
    fprintf(f_json, "    \"magic\": \"%s\",\n", HEADER_MAGIC);
    fprintf(f_json, "    \"filesize\": \"0x%08X\",\n", filesize);
    fprintf(f_json, "    \"gamecode\": \"%.4s\",\n", gamecode);
    fprintf(f_json, "    \"version\": \"0x%02X\",\n", version);
    fprintf(f_json, "    \"rom_block_index_address\": \"0x%08X\",\n", index_addr);
    fprintf(f_json, "    \"rom_block_count\": %u,\n", block_count);
    fprintf(f_json, "    \"rom_block_index_size\": \"0x%08X\",\n", index_size);
    fprintf(f_json, "    \"rom_block_min\": 0x%04X,\n", min_block);
    fprintf(f_json, "    \"rom_block_max\": 0x%04X\n", max_block);
    fprintf(f_json, "  },\n");
    // Blocks
    fprintf(f_json, "  \"rom_blocks\": [\n");
    for (uint32_t i=0; i<block_count; i++) {
        RomBlock *b = &blocks[i];
        fprintf(f_json, "    {\n");
        fprintf(f_json, "      \"block_index\": 0x%04X,\n", b->block_id);
        fprintf(f_json, "      \"block_patch_address\": \"0x%08X\",\n", index_addr + i*16 +0);
        fprintf(f_json, "      \"block_patch_count\": %u,\n", b->record_count);
        fprintf(f_json, "      \"block_patch_size\": \"0x%08X\",\n", b->ips_size);
        fprintf(f_json, "      \"records\": [\n");
        for (uint32_t j=0; j<b->record_count; j++) {
            IPSRecord *r = &b->records[j];
            fprintf(f_json, "        { \"offset\": \"0x%06X\"", r->offset);
            if (r->size ==0) {
                fprintf(f_json, ", \"rle_length\": \"0x%04X\", \"rle_byte\": \"0x%02X\"", r->d.rle.rle_len, r->d.rle.rle_byte);
            } else {
                fprintf(f_json, ", \"size\": \"0x%04X\", \"data\": [", r->size);
                for (uint32_t k=0; k<r->size; k++) {
                    if (k>0) fprintf(f_json, ", ");
                    fprintf(f_json, "\"%02X\"", r->d.std.data[k]);
                }
                fprintf(f_json, "]");
            }
            if (j < b->record_count -1) fprintf(f_json, " },\n");
            else fprintf(f_json, " }\n");
        }
        fprintf(f_json, "      ]\n");
        if (i < block_count -1) fprintf(f_json, "    },\n");
        else fprintf(f_json, "    }\n");
    }
    fprintf(f_json, "  ],\n");
    fprintf(f_json, "  \"footer\": {\n");
    fprintf(f_json, "    \"magic\": \"%s\"\n", FOOTER_MAGIC);
    fprintf(f_json, "  }\n");
    fprintf(f_json, "}\n");
    fclose(f_json);
    printf("JSON file created: %s\n", json_path);
cleanup:
    free(patch_buf);
    for (uint32_t i=0; i<block_count; i++) {
        for (uint32_t j=0; j<blocks[i].record_count; j++) {
            if (blocks[i].records[j].size !=0) free(blocks[i].records[j].d.std.data);
        }
        free(blocks[i].records);
    }
    free(blocks);
    return 0;
}
// Patch to ips mode
static int mode_patch2ips(const char *patch_path) {
    printf("Converting patch to IPS: %s...\n", patch_path);
    // Check suffix
    const char *ext = strrchr(patch_path, '.');
    if (!ext || strcmp(ext, ".patch") !=0) {
        fprintf(stderr, "Warning: Patch file does not have .patch suffix\n");
    }
    char ips_path[256];
    strncpy(ips_path, patch_path, sizeof(ips_path)-1);
    char *ext_p = strrchr(ips_path, '.');
    if (ext_p) {
        snprintf(ext_p, sizeof(ips_path) - (ext_p - ips_path), ".ips");
    } else {
        strcat(ips_path, ".ips");
    }
    // Read patch
    FILE *f_patch = fopen(patch_path, "rb");
    if (!f_patch) {
        perror("Cannot open patch file");
        return -1;
    }
    fseek(f_patch,0,SEEK_END);
    long patch_size = ftell(f_patch);
    fseek(f_patch,0,SEEK_SET);
    uint8_t *patch_buf = malloc(patch_size);
    fread(patch_buf,1,patch_size,f_patch);
    fclose(f_patch);
    // Verify
    if (memcmp(patch_buf, HEADER_MAGIC,8) !=0) {
        fprintf(stderr, "Invalid patch file\n");
        free(patch_buf);
        return -1;
    }
    uint32_t index_addr = read_le32(&patch_buf[20]);
    uint32_t block_count = read_le32(&patch_buf[24]);
    // Check if any block >=4096 (16MB)
    for (uint32_t i=0; i<block_count; i++) {
        uint8_t *idx = &patch_buf[index_addr + i*16];
        uint32_t b_id = read_le32(&idx[12]);
        if (b_id >=4096) {
            fprintf(stderr, "Error: Patch contains rom block 0x%04X which is beyond 16MB, cannot convert to IPS\n", b_id);
            free(patch_buf);
            return -1;
        }
    }
    // Collect all records
    IPSRecord *all_records = NULL;
    uint32_t all_count =0, all_cap=0;
    // Parse block records
    for (uint32_t i=0; i<block_count; i++) {
        uint8_t *idx = &patch_buf[index_addr + i*16];
        uint32_t b_addr = read_le32(&idx[0]);
        uint32_t b_size = read_le32(&idx[8]);
        uint32_t b_id = read_le32(&idx[12]);
        uint32_t b_base = b_id * BLOCK_SIZE;
        IPSRecord *recs;
        uint32_t cnt;
        parse_ips_data(&patch_buf[b_addr], b_size, &recs, &cnt);
        for (uint32_t j=0; j<cnt; j++) {
            IPSRecord r = recs[j];
            r.offset = b_base + r.offset; // convert to absolute
            add_record_to_array(&all_records, &all_count, &all_cap, r);
        }
        free(recs);
    }
    // Sort records by offset
    qsort(all_records, all_count, sizeof(IPSRecord), compare_ips_records);
    // Generate ips
    uint8_t *ips_data;
    uint32_t ips_size;
    generate_ips_data(all_records, all_count, &ips_data, &ips_size);
    // Write
    FILE *f_ips = fopen(ips_path, "wb");
    if (!f_ips) {
        perror("Cannot create ips file");
        goto cleanup;
    }
    fwrite(ips_data,1,ips_size,f_ips);
    fclose(f_ips);
    printf("IPS file created: %s\n", ips_path);
cleanup:
    free(patch_buf);
    for (uint32_t i=0; i<all_count; i++) {
        if (all_records[i].size !=0) free(all_records[i].d.std.data);
    }
    free(all_records);
    free(ips_data);
    return 0;
}
// Split record into blocks
static void split_record(uint32_t abs_addr, IPSRecord rec, 
                        RomBlock **blocks, uint32_t *block_count, uint32_t *block_cap) {
    uint32_t len;
    if (rec.size ==0) {
        len = rec.d.rle.rle_len;
    } else {
        len = rec.size;
    }
    // Split all records into 4KB blocks
    uint32_t current_addr = abs_addr;
    uint32_t remaining = len;
    while (remaining >0) {
        uint32_t block_id = current_addr / BLOCK_SIZE;
        uint32_t block_offset = current_addr % BLOCK_SIZE;
        uint32_t can_copy = remaining;
        if (block_offset + can_copy > BLOCK_SIZE) {
            can_copy = BLOCK_SIZE - block_offset;
        }
        IPSRecord new_rec;
        // All regions use block relative offset
        new_rec.offset = block_offset;
        
        if (rec.size ==0) {
            // RLE record, split into small RLE or standard
            if (can_copy >=3) {
                new_rec.size =0;
                new_rec.d.rle.rle_len = can_copy;
                new_rec.d.rle.rle_byte = rec.d.rle.rle_byte;
            } else {
                new_rec.size = can_copy;
                new_rec.d.std.data = malloc(can_copy);
                for (uint32_t i=0; i<can_copy; i++) {
                    new_rec.d.std.data[i] = rec.d.rle.rle_byte;
                }
            }
        } else {
            new_rec.size = can_copy;
            new_rec.d.std.data = malloc(can_copy);
            memcpy(new_rec.d.std.data, &rec.d.std.data[current_addr - abs_addr], can_copy);
        }
        // Add to correct block
        RomBlock *block = NULL;
        for (uint32_t i=0; i<*block_count; i++) {
            if ((*blocks)[i].block_id == block_id) {
                block = &(*blocks)[i];
                break;
            }
        }
        if (!block) {
            RomBlock new_block = {0};
            new_block.block_id = block_id;
            add_block_to_array(blocks, block_count, block_cap, new_block);
            block = &(*blocks)[*block_count -1];
        }
        add_record_to_array(&block->records, &block->record_count, &block->record_capacity, new_rec);
        
        current_addr += can_copy;
        remaining -= can_copy;
    }
}
// Ips to patch mode
static int mode_ips2patch(const char *ips_path) {
    printf("Converting IPS to patch: %s...\n", ips_path);
    // Check suffix
    const char *ext = strrchr(ips_path, '.');
    if (!ext || strcmp(ext, ".ips") !=0) {
        fprintf(stderr, "Warning: IPS file does not have .ips suffix\n");
    }
    char patch_path[256];
    strncpy(patch_path, ips_path, sizeof(patch_path)-1);
    char *ext_p = strrchr(patch_path, '.');
    if (ext_p) {
        snprintf(ext_p, sizeof(patch_path) - (ext_p - patch_path), ".patch");
    } else {
        strcat(patch_path, ".patch");
    }
    // Read ips
    FILE *f_ips = fopen(ips_path, "rb");
    if (!f_ips) {
        perror("Cannot open ips file");
        return -1;
    }
    fseek(f_ips,0,SEEK_END);
    long ips_size = ftell(f_ips);
    fseek(f_ips,0,SEEK_SET);
    uint8_t *ips_buf = malloc(ips_size);
    fread(ips_buf,1,ips_size,f_ips);
    fclose(f_ips);
    // Parse ips
    IPSRecord *ips_records;
    uint32_t ips_count;
    if (parse_ips_data(ips_buf, ips_size, &ips_records, &ips_count) !=0) {
        fprintf(stderr, "Invalid IPS file\n");
        free(ips_buf);
        return -1;
    }
    // Split records
    RomBlock *blocks = NULL;
    uint32_t block_count=0, block_cap=0;
    for (uint32_t i=0; i<ips_count; i++) {
        uint32_t abs_addr = ips_records[i].offset;
        split_record(abs_addr, ips_records[i], 
                    &blocks, &block_count, &block_cap);
    }
    // Generate ips data
    for (uint32_t i=0; i<block_count; i++) {
        if (blocks[i].record_count >0) {
            generate_ips_data(blocks[i].records, blocks[i].record_count, 
                             &blocks[i].ips_data, &blocks[i].ips_size);
        }
    }
    // Build patch file
    uint32_t header_size=36;
    uint32_t current_pos = header_size;
    uint32_t idx_addr=0;
    uint32_t idx_size = block_count *16;
    if (block_count>0) {
        idx_addr = current_pos;
        current_pos = align16(current_pos + idx_size);
    }
    uint32_t *block_addrs = malloc(block_count * sizeof(uint32_t));
    for (uint32_t i=0; i<block_count; i++) {
        block_addrs[i] = current_pos;
        current_pos = align16(current_pos + blocks[i].ips_size);
    }
    uint32_t footer_pos = align16(current_pos);
    uint32_t total_size = footer_pos +6;
    // Header
    uint8_t header[36] = {0};
    memcpy(header, HEADER_MAGIC,8);
    write_le32(&header[8], total_size);
    // Default gamecode UNKN, version 0
    memcpy(&header[12], "UNKN",4);
    header[16] =0;
    write_le32(&header[20], idx_addr);
    write_le32(&header[24], block_count);
    write_le32(&header[28], idx_size);
    uint16_t min_block=0, max_block=0;
    if (block_count>0) {
        min_block = blocks[0].block_id;
        max_block = blocks[0].block_id;
        for (uint32_t i=1; i<block_count; i++) {
            if (blocks[i].block_id < min_block) min_block = blocks[i].block_id;
            if (blocks[i].block_id > max_block) max_block = blocks[i].block_id;
        }
    }
    write_le16(&header[32], min_block);
    write_le16(&header[34], max_block);
    // Write file
    FILE *f_patch = fopen(patch_path, "wb");
    if (!f_patch) {
        perror("Cannot create patch file");
        goto cleanup;
    }
    fwrite(header,1,36,f_patch);
    if (block_count>0) {
        for (uint32_t i=0; i<block_count; i++) {
            uint8_t idx[16];
            write_le32(&idx[0], block_addrs[i]);
            write_le32(&idx[4], blocks[i].record_count);
            write_le32(&idx[8], blocks[i].ips_size);
            write_le32(&idx[12], blocks[i].block_id);
            fwrite(idx,1,16,f_patch);
        }
        uint32_t pad = align16(idx_addr + idx_size) - (idx_addr + idx_size);
        uint8_t zero[16]={0};
        fwrite(zero,1,pad,f_patch);
    }
    for (uint32_t i=0; i<block_count; i++) {
        fwrite(blocks[i].ips_data,1, blocks[i].ips_size, f_patch);
        uint32_t pad = align16(block_addrs[i] + blocks[i].ips_size) - (block_addrs[i] + blocks[i].ips_size);
        uint8_t zero[16]={0};
        fwrite(zero,1,pad,f_patch);
    }
    uint32_t pad = footer_pos - current_pos;
    uint8_t zero[16]={0};
    fwrite(zero,1,pad,f_patch);
    fwrite(FOOTER_MAGIC,1,6,f_patch);
    fclose(f_patch);
    printf("Patch file created: %s\n", patch_path);
cleanup:
    free(ips_buf);
    for (uint32_t i=0; i<ips_count; i++) {
        if (ips_records[i].size !=0) free(ips_records[i].d.std.data);
    }
    free(ips_records);
    for (uint32_t i=0; i<block_count; i++) {
        for (uint32_t j=0; j<blocks[i].record_count; j++) {
            if (blocks[i].records[j].size !=0) free(blocks[i].records[j].d.std.data);
        }
        free(blocks[i].records);
        free(blocks[i].ips_data);
    }
    free(blocks);
    free(block_addrs);
    return 0;
}
void print_usage() {
    printf("Usage: PatchToolForGbaR3 <mode> <input1> [input2] [output]\n");
    printf("Modes:\n");
    printf("  -c/create <orig_rom> <new_rom> [out_patch] - Create patch file\n");
    printf("  -a/apply <patch_file> <orig_rom> [out_rom] - Apply patch to rom\n");

    printf("  -p2j/patch2json <patch_file> - Convert patch to json\n");
    printf("  -i2p/ips2patch <ips_file> - Convert ips to patch\n");
    printf("  -p2i/patch2ips <patch_file> - Convert patch to ips\n");
    
    printf("  automode - drag .patch/.patch.json/.ips to this exe to auto use -p2j/-p2i/i2p\n");
}
int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }
    const char *mode = argv[1];
    if (strcmp(mode, "-c") ==0) {
        if (argc <4) {
            print_usage();
            return 1;
        }
        return mode_create(argv[2], argv[3], argc>4 ? argv[4] : NULL);
    } else if (strcmp(mode, "-a") ==0) {
        if (argc <4) {
            print_usage();
            return 1;
        }
        return mode_apply(argv[2], argv[3], argc>4 ? argv[4] : NULL);

    } else if (strcmp(mode, "-p2j") ==0) {
        if (argc <3) {
            print_usage();
            return 1;
        }
        return mode_patch2json(argv[2]);
    } else if (strcmp(mode, "-i2p") ==0) {
        if (argc <3) {
            print_usage();
            return 1;
        }
        return mode_ips2patch(argv[2]);
    } else if (strcmp(mode, "-p2i") ==0) {
        if (argc <3) {
            print_usage();
            return 1;
        }
        return mode_patch2ips(argv[2]);
    } else if (strcmp(mode, "patch2json") == 0) {
        if (argc < 3) {
            print_usage();
            return 1;
        }
        return mode_patch2json(argv[2]);
    } else if (strcmp(mode, "ips2patch") == 0) {
        if (argc < 3) {
            print_usage();
            return 1;
        }
        return mode_ips2patch(argv[2]);
    } else if (strcmp(mode, "patch2ips") == 0) {
        if (argc < 3) {
            print_usage();
            return 1;
        }
        return mode_patch2ips(argv[2]);
    } else {
        // Auto detect mode by file extension
        // Check if first file is patch (argv[1] is the first file, no mode specified)
        const char *ext1 = strrchr(argv[1], '.');
        if (ext1 && strcmp(ext1, ".patch") == 0) {
            // Patch file, convert to both ips and json at the same time
            mode_patch2ips(argv[1]);
            mode_patch2json(argv[1]);
            return 0;
        } else if (ext1 && strcmp(ext1, ".ips") == 0) {
            // Ips file, convert to patch
            mode_ips2patch(argv[1]);
            return 0;
        } else {
            printf("Unknown mode or cannot auto detect mode\n");
            print_usage();
            return 1;
        }
    }
    return 0;
}
