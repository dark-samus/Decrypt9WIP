#include "fs.h"
#include "draw.h"
#include "hid.h"
#include "platform.h"
#include "gamecart/protocol.h"
#include "gamecart/command_ctr.h"
#include "decryptor/aes.h"
#include "decryptor/sha.h"
#include "decryptor/decryptor.h"
#include "decryptor/hashfile.h"
#include "decryptor/keys.h"
#include "decryptor/titlekey.h"
#include "decryptor/game.h"

#define CART_CHUNK_SIZE (u32) (1*1024*1024)


u32 GetSdCtr(u8* ctr, const char* path)
{
    // get AES counter, see: http://www.3dbrew.org/wiki/Extdata#Encryption
    // path is the part of the full path after //Nintendo 3DS/<ID0>/<ID1>
    u8 hashstr[256];
    u8 sha256sum[32];
    u32 plen = 0;
    // poor man's UTF-8 -> UTF-16
    for (u32 i = 0; i < 128; i++) {
        hashstr[2*i] = path[i];
        hashstr[2*i+1] = 0;
        if (path[i] == 0) {
            plen = i;
            break;
        }
    }
    sha_quick(sha256sum, hashstr, (plen + 1) * 2, SHA256_MODE);
    for (u32 i = 0; i < 16; i++)
        ctr[i] = sha256sum[i] ^ sha256sum[i+16];
    
    return 0;
}

u32 GetNcchCtr(u8* ctr, NcchHeader* ncch, u8 sub_id) {
    memset(ctr, 0x00, 16);
    if (ncch->version == 1) {
        memcpy(ctr, &(ncch->partitionId), 8);
        if (sub_id == 1) { // exHeader ctr
            add_ctr(ctr, 0x200); 
        } else if (sub_id == 2) { // exeFS ctr
            add_ctr(ctr, ncch->offset_exefs * 0x200);
        } else if (sub_id == 3) { // romFS ctr
            add_ctr(ctr, ncch->offset_romfs * 0x200);
        }
    } else {
        for (u32 i = 0; i < 8; i++)
            ctr[i] = ((u8*) &(ncch->partitionId))[7-i];
        ctr[8] = sub_id;
    }
    
    return 0;
}

u32 SdFolderSelector(char* path, u8* keyY)
{
    char** dirptr = (char**) 0x20400000; // allow using 0x8000 byte
    char* dirlist = (char*) 0x20408000; // allow using 0x80000 byte
    u32 n_dirs = 0;
    
    // the keyY is used to generate a proper base path
    // see here: https://www.3dbrew.org/wiki/Nand/private/movable.sed
    u32 sha256sum[8];
    char base_path[64];
    sha_quick(sha256sum, keyY, 16, SHA256_MODE);
    snprintf(base_path, 63, "/Nintendo 3DS/%08X%08X%08X%08X",
        (unsigned int) sha256sum[0], (unsigned int) sha256sum[1],
        (unsigned int) sha256sum[2], (unsigned int) sha256sum[3]);
    Debug("<id0> is %s", base_path + 14);
    if (!GetFileList(base_path, dirlist, 0x80000, true, false, true)) {
        Debug("Failed retrieving the dirlist");
        return 1;
    }
    
    // parse the dirlist for usable entries
    for (char* dir = strtok(dirlist, "\n"); dir != NULL; dir = strtok(NULL, "\n")) {
        if (strnlen(dir, 256) <= 13 + 33 + 33)
            continue;
        if (strchrcount(dir, '/') > 6)
            continue; // allow a maximum depth of 6 for the full folder
        char* subdir = dir + 13 + 33 + 33; // length of ("/Nintendo 3DS" + "/<id0>" + "/<id1>");
        if ((strncmp(subdir, "/dbs", 4) != 0) && (strncmp(subdir, "/extdata", 8) != 0) && (strncmp(subdir, "/title", 6) != 0))
            continue;
        dirptr[n_dirs++] = dir;
        if (n_dirs * sizeof(char**) >= 0x8000)
            return 1;
    }
    if (n_dirs == 0) {
        Debug("No valid SD data found");
        return 1;
    }
    
    // let the user choose a directory
    u32 index = 0;
    strncpy(path, dirptr[0], 128);
    Debug("Use arrow keys and <A> to choose a folder");
    while (true) {
        Debug("\r%s", path + 13 + 33 + 33);
        u32 pad_state = InputWait();
        u32 cur_lvl = strchrcount(path, '/');
        if (pad_state & BUTTON_DOWN) { // find next path of same level
            do {
                if (++index >= n_dirs)
                    index = 0;
            } while (strchrcount(dirptr[index], '/') != cur_lvl);
        } else if (pad_state & BUTTON_UP) { // find prev path of same level
            do {
                index = (index) ? index - 1 : n_dirs - 1;
            } while (strchrcount(dirptr[index], '/') != cur_lvl);
        } else if ((pad_state & BUTTON_RIGHT) && (cur_lvl < 6)) { // up one level
            if ((index < n_dirs - 1) && (strchrcount(dirptr[index+1], '/') > cur_lvl))
                index++; // this only works because of the sorting of the dir list
        } else if ((pad_state & BUTTON_LEFT) && (cur_lvl > 4)) { // down one level
            while ((index > 0) && (cur_lvl == strchrcount(dirptr[index], '/')))
                index--;
        } else if (pad_state & BUTTON_A) {
            Debug("%s", path + 13 + 33 + 33);
            break;
        } else if (pad_state & BUTTON_B) {
            Debug("(cancelled by user)");
            return 2;
        }
        strncpy(path, dirptr[index], 128);
    }
    
    return 0;
}

u32 CryptSdToSd(const char* filename, u32 offset, u32 size, CryptBufferInfo* info, bool handle_offset16)
{
    u8* buffer = BUFFER_ADDRESS;
    u32 offset_16 = (handle_offset16) ? offset % 16 : 0;
    u32 result = 0;

    // no DebugFileOpen() - at this point the file has already been checked enough
    if (!FileOpen(filename)) 
        return 1;

    info->buffer = buffer;
    if (offset_16) { // handle offset alignment / this assumes the data is >= 16 byte
        if(!DebugFileRead(buffer + offset_16, 16 - offset_16, offset)) {
            result = 1;
        }
        info->size = 16;
        CryptBuffer(info);
        if(!DebugFileWrite(buffer + offset_16, 16 - offset_16, offset)) {
            result = 1;
        }
    }
    for (u32 i = (offset_16) ? (16 - offset_16) : 0; i < size; i += BUFFER_MAX_SIZE) {
        u32 read_bytes = min(BUFFER_MAX_SIZE, (size - i));
        ShowProgress(i, size);
        if(!DebugFileRead(buffer, read_bytes, offset + i)) {
            result = 1;
            break;
        }
        info->size = read_bytes;
        CryptBuffer(info);
        if(!DebugFileWrite(buffer, read_bytes, offset + i)) {
            result = 1;
            break;
        }
    }

    ShowProgress(0, 0);
    FileClose();

    return result;
}

u32 VerifyNcch(const char* filename, u32 offset)
{
    NcchHeader* ncch = (NcchHeader*) 0x20316200;
    u8* exefs = (u8*) 0x20316400;
    char* status_str[3] = { "OK", "Fail", "-" }; 
    u32 ver_exthdr = 2;
    u32 ver_exefs = 2;
    u32 ver_romfs = 2;
    
    // some basic checks included - this only verifies decrypted NCCHs
    if (FileGetData(filename, (void*) ncch, 0x200, offset) != 0x200)
        return 1;
    if ((memcmp(ncch->magic, "NCCH", 4) != 0) || (!(ncch->flags[7] & 0x04)))
        return 1;

    // base hash checks for ExHeader / ExeFS / RomFS
    if (ncch->size_exthdr > 0)
        ver_exthdr = CheckHashFromFile(filename, offset + 0x200, 0x400, ncch->hash_exthdr);
    if (ncch->size_exefs_hash > 0)
        ver_exefs = CheckHashFromFile(filename, offset + (ncch->offset_exefs * 0x200), ncch->size_exefs_hash * 0x200, ncch->hash_exefs);
    if (ncch->size_romfs_hash > 0)
        ver_romfs = CheckHashFromFile(filename, offset + (ncch->offset_romfs * 0x200), ncch->size_romfs_hash * 0x200, ncch->hash_romfs);
    
    // thorough exefs verification
    if (ncch->size_exefs > 0) {
        u32 offset_byte = ncch->offset_exefs * 0x200;
        if (FileGetData(filename, exefs, 0x200, offset + offset_byte) != 0x200)
            ver_exefs = 1;
        for (u32 i = 0; (i < 10) && (ver_exefs != 1); i++) {
            u32 offset_exefs_file = offset_byte + getle32(exefs + (i*0x10) + 0x8) + 0x200;
            u32 size_exefs_file = getle32(exefs + (i*0x10) + 0xC);
            u8* hash_exefs_file = exefs + 0x200 - ((i+1)*0x20);
            if (size_exefs_file == 0)
                break;
            ver_exefs = CheckHashFromFile(filename, offset + offset_exefs_file, size_exefs_file, hash_exefs_file);
        }
    }
    
    // output results
    Debug("Verify ExHdr/ExeFS/RomFS: %s/%s/%s", status_str[ver_exthdr], status_str[ver_exefs], status_str[ver_romfs]);
    
    return (((ver_exthdr | ver_exefs | ver_romfs) & 1) == 0) ? 0 : 1;
}

u32 CryptNcch(const char* filename, u32 offset, u32 size, u64 seedId, u8* encrypt_flags)
{
    NcchHeader* ncch = (NcchHeader*) 0x20316200;
    u8* buffer = (u8*) 0x20316400;
    CryptBufferInfo info0 = {.setKeyY = 1, .keyslot = 0x2C, .mode = AES_CNT_CTRNAND_MODE};
    CryptBufferInfo info1 = {.setKeyY = 1, .mode = AES_CNT_CTRNAND_MODE};
    u8 seedKeyY[16] = { 0x00 };
    u32 result = 0;
    
    if (FileGetData(filename, (void*) ncch, 0x200, offset) != 0x200)
        return 1; // it's impossible to fail here anyways
 
    // check (again) for magic number
    if (memcmp(ncch->magic, "NCCH", 4) != 0) {
        Debug("Not a NCCH container");
        return 2; // not an actual error
    }
    
    // size plausibility check
    u32 size_sum = 0x200 + ((ncch->size_exthdr) ? 0x800 : 0x0) + 0x200 *
        (ncch->size_plain + ncch->size_logo + ncch->size_exefs + ncch->size_romfs);
    if (ncch->size * 0x200 < size_sum) {
        Debug("Probably not a NCCH container");
        return 2; // not an actual error
    }        
    
    // check if encrypted
    if (!encrypt_flags && (ncch->flags[7] & 0x04)) {
        Debug("NCCH is not encrypted");
        return 2; // not an actual error
    } else if (encrypt_flags && !(ncch->flags[7] & 0x04)) {
        Debug("NCCH is already encrypted");
        return 2; // not an actual error
    } else if (encrypt_flags && (encrypt_flags[7] & 0x04)) {
        Debug("Nothing to do!");
        return 2; // not an actual error
    }
    
    // check size
    if ((size > 0) && (ncch->size * 0x200 > size)) {
        Debug("NCCH size is out of bounds");
        return 1;
    }
    
    // select correct title ID for seed crypto
    if (seedId == 0) seedId = ncch->programId;
    
    // copy over encryption parameters (if applicable)
    if (encrypt_flags) {
        ncch->flags[3] = encrypt_flags[3];
        ncch->flags[7] &= (0x01|0x20|0x04)^0xFF;
        ncch->flags[7] |= (0x01|0x20)&encrypt_flags[7];
    }
    
    // check crypto type
    bool uses7xCrypto = ncch->flags[3];
    bool usesSeedCrypto = ncch->flags[7] & 0x20;
    bool usesSec3Crypto = (ncch->flags[3] == 0x0A);
    bool usesSec4Crypto = (ncch->flags[3] == 0x0B);
    bool usesFixedKey = ncch->flags[7] & 0x01;
    
    Debug("Code / Crypto: %.16s / %s%s%s%s", ncch->productCode, (usesFixedKey) ? "FixedKey " : "", (usesSec4Crypto) ? "Secure4 " : (usesSec3Crypto) ? "Secure3 " : (uses7xCrypto) ? "7x " : "", (usesSeedCrypto) ? "Seed " : "", (!uses7xCrypto && !usesSeedCrypto && !usesFixedKey) ? "Standard" : "");
    
    // setup zero key crypto
    if (usesFixedKey) {
        // from https://github.com/profi200/Project_CTR/blob/master/makerom/pki/dev.h
        u8 zeroKey[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        u8 sysKey[16]  = {0x52, 0x7C, 0xE6, 0x30, 0xA9, 0xCA, 0x30, 0x5F, 0x36, 0x96, 0xF3, 0xCD, 0xE9, 0x54, 0x19, 0x4B};
        uses7xCrypto = usesSeedCrypto = usesSec3Crypto = usesSec4Crypto = false;
        info1.setKeyY = info0.setKeyY = 0;
        info1.keyslot = info0.keyslot = 0x11;
        setup_aeskey(0x11, (ncch->programId & ((u64) 0x10 << 32)) ? sysKey : zeroKey);
    }
    
    // check 7x crypto
    if (uses7xCrypto && (CheckKeySlot(0x25, 'X') != 0)) {
        Debug("slot0x25KeyX not set up");
        Debug("This won't work on O3DS < 7.x or A9LH");
        return 1;
    }
    
    // check Secure3 crypto on O3DS
    if (usesSec3Crypto && (CheckKeySlot(0x18, 'X') != 0)) {
        Debug("slot0x18KeyX not set up");
        Debug("Secure3 crypto is not available");
        return 1;
    }
    
    // check Secure4 crypto
    if (usesSec4Crypto && (CheckKeySlot(0x1B, 'X') != 0)) {
        Debug("slot0x1BKeyX not set up");
        Debug("Secure4 crypto is not available");
        return 1;
    }
    
    // check / setup seed crypto
    if (usesSeedCrypto) {
        if (FileOpen("seeddb.bin")) {
            SeedInfoEntry* entry = (SeedInfoEntry*) buffer;
            u32 found = 0;
            for (u32 i = 0x10;; i += 0x20) {
                if (FileRead(entry, 0x20, i) != 0x20)
                    break;
                if (entry->titleId == seedId) {
                    u8 keydata[32];
                    memcpy(keydata, ncch->signature, 16);
                    memcpy(keydata + 16, entry->external_seed, 16);
                    u8 sha256sum[32];
                    sha_quick(sha256sum, keydata, 32, SHA256_MODE);
                    memcpy(seedKeyY, sha256sum, 16);
                    found = 1;
                }
            }
            FileClose();
            if (!found) {
                Debug("Seed not found in seeddb.bin!");
                return 1;
            }
        } else {
            Debug("Need seeddb.bin for seed crypto!");
            return 1;
        }
        Debug("Loading seed from seeddb.bin: ok");
    }
    
    // basic setup of CryptBufferInfo structs
    memcpy(info0.keyY, ncch->signature, 16);
    memcpy(info1.keyY, (usesSeedCrypto) ? seedKeyY : ncch->signature, 16);
    info1.keyslot = (usesSec4Crypto) ? 0x1B : ((usesSec3Crypto) ? 0x18 : ((uses7xCrypto) ? 0x25 : info0.keyslot));
    
    Debug("%s ExHdr/ExeFS/RomFS (%ukB/%ukB/%uMB)",
        (encrypt_flags) ? "Encrypt" : "Decrypt",
        (ncch->size_exthdr > 0) ? 0x800 / 1024 : 0,
        (ncch->size_exefs * 0x200) / 1024,
        (ncch->size_romfs * 0x200) / (1024*1024));
        
    // process ExHeader
    if (ncch->size_exthdr > 0) {
        GetNcchCtr(info0.ctr, ncch, 1);
        result |= CryptSdToSd(filename, offset + 0x200, 0x800, &info0, true);
    }
    
    // process ExeFS
    if (ncch->size_exefs > 0) {
        u32 offset_byte = ncch->offset_exefs * 0x200;
        u32 size_byte = ncch->size_exefs * 0x200;
        if (uses7xCrypto || usesSeedCrypto) {
            GetNcchCtr(info0.ctr, ncch, 2);
            if (!encrypt_flags) // decrypt this first (when decrypting)
                result |= CryptSdToSd(filename, offset + offset_byte, 0x200, &info0, true);
            if (FileGetData(filename, buffer, 0x200, offset + offset_byte) != 0x200) // get exeFS header
                return 1;
            if (encrypt_flags) // encrypt this last (when encrypting)
                result |= CryptSdToSd(filename, offset + offset_byte, 0x200, &info0, true);
            // special ExeFS decryption routine ("banner" and "icon" use standard crypto)
            for (u32 i = 0; i < 10; i++) {
                char* name_exefs_file = (char*) buffer + (i*0x10);
                u32 offset_exefs_file = getle32(buffer + (i*0x10) + 0x8) + 0x200;
                u32 size_exefs_file = getle32(buffer + (i*0x10) + 0xC);
                CryptBufferInfo* infoExeFs = ((strncmp(name_exefs_file, "banner", 8) == 0) ||
                    (strncmp(name_exefs_file, "icon", 8) == 0)) ? &info0 : &info1;
                if (size_exefs_file == 0)
                    continue;
                if (offset_exefs_file % 16) {
                    Debug("ExeFS file offset not aligned!");
                    result |= 1;
                    break; // this should not happen
                }
                GetNcchCtr(infoExeFs->ctr, ncch, 2);
                add_ctr(infoExeFs->ctr, offset_exefs_file / 0x10);
                infoExeFs->setKeyY = 1;
                result |= CryptSdToSd(filename, offset + offset_byte + offset_exefs_file,
                    align(size_exefs_file, 16), infoExeFs, true);
            }
        } else {
            GetNcchCtr(info0.ctr, ncch, 2);
            result |= CryptSdToSd(filename, offset + offset_byte, size_byte, &info0, true);
        }
    }
    
    // process RomFS
    if (ncch->size_romfs > 0) {
        GetNcchCtr(info1.ctr, ncch, 3);
        if (!usesFixedKey)
            info1.setKeyY = 1;
        result |= CryptSdToSd(filename, offset + (ncch->offset_romfs * 0x200), ncch->size_romfs * 0x200, &info1, true);
    }
    
    // set NCCH header flags
    if (!encrypt_flags) {
        ncch->flags[3] = 0x00;
        ncch->flags[7] &= (0x01|0x20)^0xFF;
        ncch->flags[7] |= 0x04;
    }
    
    // write header back
    if (!FileOpen(filename))
        return 1;
    if (!DebugFileWrite((void*) ncch, 0x200, offset)) {
        FileClose();
        return 1;
    }
    FileClose();
    
    
    return ((result == 0) && !encrypt_flags) ? VerifyNcch(filename, offset) : result;
}

u32 CryptCia(const char* filename, u8* ncch_crypt, bool cia_encrypt, bool cxi_only)
{
    u8* buffer = (u8*) 0x20316600;
    __attribute__((aligned(16))) u8 titlekey[16];
    u64 titleId;
    u8* content_list;
    u8* ticket_data;
    u8* tmd_data;
    
    u32 offset_ticktmd;
    u32 offset_content;    
    u32 size_ticktmd;
    u32 size_ticket;
    u32 size_tmd;
    u32 size_content;
    
    u32 content_count;
    u32 result = 0;
    
    if (cia_encrypt) // process only one layer when encrypting
        ncch_crypt = NULL;
    
    if (!FileOpen(filename)) // already checked this file
        return 1;
    if (!DebugFileRead(buffer, 0x20, 0x00)) {
        FileClose();
        return 1;
    }
    
    // get offsets for various sections & check
    u32 section_size[6];
    u32 section_offset[6];
    section_size[0] = getle32(buffer);
    section_offset[0] = 0;
    for (u32 i = 1; i < 6; i++) {
        section_size[i] = getle32(buffer + 4 + ((i == 4) ? (5*4) : (i == 5) ? (4*4) : (i*4)) );
        section_offset[i] = section_offset[i-1] + align(section_size[i-1], 64);
    }
    offset_ticktmd = section_offset[2];
    offset_content = section_offset[4];
    size_ticktmd = section_offset[4] - section_offset[2];
    size_ticket = section_size[2];
    size_tmd = section_size[3];
    size_content = section_size[4];
    
    if (FileGetSize() < section_offset[5] + align(section_size[5], 64)) {
        Debug("Not a CIA or corrupt file");
        FileClose();
        return 1;
    }
    
    if ((size_ticktmd) > 0x10000) {
        Debug("Ticket/TMD too big");
        FileClose();
        return 1;
    }
    
    // load ticket & tmd to buffer, close file
    if (!DebugFileRead(buffer, size_ticktmd, offset_ticktmd)) {
        FileClose();
        return 1;
    }
    FileClose();
    
    u32 signature_size[2] = { 0 };
    u8* section_data[2] = {buffer, buffer + align(size_ticket, 64)};
    for (u32 i = 0; i < 2; i++) {
        u32 type = section_data[i][3];
        signature_size[i] = (type == 3) ? 0x240 : (type == 4) ? 0x140 : (type == 5) ? 0x80 : 0;         
        if ((signature_size[i] == 0) || (memcmp(section_data[i], "\x00\x01\x00", 3) != 0)) {
            Debug("Unknown signature type: %08X", getbe32(section_data[i]));
            return 1;
        }
    }
    
    ticket_data = section_data[0] + signature_size[0];
    size_ticket -= signature_size[0];
    tmd_data = section_data[1] + signature_size[1];
    size_tmd -= signature_size[1];
    
    // extract & decrypt titlekey
    if (size_ticket < 0x210) {
        Debug("Ticket is too small (%i byte)", size_ticket);
        return 1;
    }
    TitleKeyEntry titlekeyEntry;
    titleId = getbe64(ticket_data + 0x9C);
    memcpy(titlekeyEntry.titleId, ticket_data + 0x9C, 8);
    memcpy(titlekeyEntry.titleKey, ticket_data + 0x7F, 16);
    titlekeyEntry.commonKeyIndex = *(ticket_data + 0xB1);
    CryptTitlekey(&titlekeyEntry, false);
    memcpy(titlekey, titlekeyEntry.titleKey, 16);
    
    // get content data from TMD
    content_count = getbe16(tmd_data + 0x9E);
    content_list = tmd_data + 0xC4 + (64 * 0x24);
    if (content_count * 0x30 != size_tmd - (0xC4 + (64 * 0x24))) {
        Debug("TMD content count (%i) / list size mismatch", content_count);
        return 1;
    }
    u32 size_tmd_content = 0;
    for (u32 i = 0; i < content_count; i++)
        size_tmd_content += getbe32(content_list + (0x30 * i) + 0xC);
    if (size_tmd_content != size_content) {
        Debug("TMD content size / actual size mismatch");
        return 1;
    }
    
    bool untouched = true;
    u32 n_processed = 0;
    u32 next_offset = offset_content;
    CryptBufferInfo info = {.setKeyY = 0, .keyslot = 0x11, .mode = (cia_encrypt) ? AES_CNT_TITLEKEY_ENCRYPT_MODE : AES_CNT_TITLEKEY_DECRYPT_MODE};
    setup_aeskey(0x11, titlekey);
    
    if (ncch_crypt)
        Debug("Pass #1: CIA decryption...");
    if (cxi_only) content_count = 1;
    for (u32 i = 0; i < content_count; i++) {
        u32 size = getbe32(content_list + (0x30 * i) + 0xC);
        u32 offset = next_offset;
        next_offset = offset + size;
        if (!(content_list[(0x30 * i) + 0x7] & 0x1) != cia_encrypt)
            continue; // depending on 'cia_encrypt' setting: not/already encrypted
        untouched = false;
        if (cia_encrypt) {
            Debug("Verifying unencrypted content...");
            if (CheckHashFromFile(filename, offset, size, content_list + (0x30 * i) + 0x10) != 0) {
                Debug("Verification failed!");
                result = 1;
                continue;
            }
            Debug("Verified OK!");
        }
        Debug("%scrypting Content %i of %i (%iMB)...", (cia_encrypt) ? "En" : "De", i + 1, content_count, size / (1024*1024));
        memset(info.ctr, 0x00, 16);
        memcpy(info.ctr, content_list + (0x30 * i) + 4, 2);
        if (CryptSdToSd(filename, offset, size, &info, true) != 0) {
            Debug("%scryption failed!", (cia_encrypt) ? "En" : "De");
            result = 1;
            continue;
        }
        if (!cia_encrypt) {
            Debug("Verifying decrypted content...");
            if (CheckHashFromFile(filename, offset, size, content_list + (0x30 * i) + 0x10) != 0) {
                Debug("Verification failed!");
                result = 1;
                continue;
            }
            Debug("Verified OK!");
        }
        content_list[(0x30 * i) + 0x7] ^= 0x1;
        n_processed++;
    }
    
    if (ncch_crypt && (result == 0)) {
        Debug("Pass #2: NCCH decryption...");
        next_offset = offset_content;
        for (u32 i = 0; i < content_count; i++) {
            u32 ncch_state;
            u32 size = getbe32(content_list + (0x30 * i) + 0xC);
            u32 offset = next_offset;
            next_offset = offset + size;
            if (content_list[(0x30 * i) + 0x7] & 0x1)
                continue; // skip this if still CIA (shallow) encrypted
            Debug("Processing Content %i of %i (%iMB)...", i + 1, content_count, size / (1024*1024));
            ncch_state = CryptNcch(filename, offset, size, titleId, NULL);
            if (!(ncch_crypt[7] & 0x04) && (ncch_state != 1))
                ncch_state = CryptNcch(filename, offset, size, titleId, ncch_crypt);
            if (ncch_state == 0) {
                untouched = false;
                Debug("Recalculating hash...");
                if (GetHashFromFile(filename, offset, size, content_list + (0x30 * i) + 0x10) != 0) {
                    Debug("Recalculation failed!");
                    result = 1;
                    continue;
                }
            } else if (ncch_state == 1) {
                Debug("Failed decrypting NCCH!");
                result = 1;
                continue;
            }
            n_processed++;
        }
        if (!untouched && (result == 0)) {
            // recalculate content info hashes
            Debug("Recalculating TMD hashes...");
            for (u32 i = 0, kc = 0; i < 64 && kc < content_count; i++) {
                u32 k = getbe16(tmd_data + 0xC4 + (i * 0x24) + 0x02);
                u8 chunk_hash[32];
                sha_quick(chunk_hash, content_list + kc * 0x30, k * 0x30, SHA256_MODE);
                memcpy(tmd_data + 0xC4 + (i * 0x24) + 0x04, chunk_hash, 32);
                kc += k;
            }
            u8 tmd_hash[32];
            sha_quick(tmd_hash, tmd_data + 0xC4, 64 * 0x24, SHA256_MODE);
            memcpy(tmd_data + 0xA4, tmd_hash, 32);
        }
    }
    
    if (untouched) {
        Debug((cia_encrypt) ? "CIA is already encrypted" : "CIA is not encrypted");
    } else if (n_processed > 0) {
        if (!FileOpen(filename)) // already checked this file
            return 1;
        if (!DebugFileWrite(buffer, size_ticktmd, offset_ticktmd))
            result = 1;
        FileClose();
    }
    
    return result;
}

u32 CryptBoss(const char* filename, bool encrypt)
{
    const u8 boss_magic[] = {0x62, 0x6F, 0x73, 0x73, 0x00, 0x01, 0x00, 0x01};
    CryptBufferInfo info = {.setKeyY = 0, .keyslot = 0x38, .mode = AES_CNT_CTRNAND_MODE};
    u8 boss_header[0x28];
    u8 content_header[0x14] = { 0x00 };
    u8 content_header_sha256[0x20];
    u8 l_sha256[0x20];
    u32 fsize = 0;
    bool encrypted = true;
    
    // base BOSS file checks
    if (!FileOpen(filename)) // already checked this file
        return 1;
    if (!DebugFileRead(boss_header, 0x28, 0x00)) {
        FileClose();
        return 1;
    }
    if (memcmp(boss_magic, boss_header, 8) != 0) {
        Debug("Not a BOSS file");
        FileClose();
        return 1;
    }
    fsize = getbe32(boss_header + 8);
    if ((fsize != FileGetSize()) || (fsize < 0x52)) {
        Debug("BOSS file has bad size");
        FileClose();
        return 1;
    }
    FileClose();

    // BOSS file verification / encryption check
    if (FileGetData(filename, content_header, 0x12, 0x28) != 0x12)
        return 1;
    if (FileGetData(filename, content_header_sha256, 0x20, 0x3A) != 0x20)
        return 1;
    sha_quick(l_sha256, content_header, 0x14, SHA256_MODE);
    encrypted = (memcmp(content_header_sha256, l_sha256, 0x20) != 0);
    if (encrypt == encrypted) {
        Debug("BOSS is already %scrypted", (encrypted) ? "en" : "de");
        return 1;
    }
    
    if (!encrypted) {
        Debug("BOSS verification: OK");
    }
    
    // BOSS file decryption
    Debug("%scrypting BOSS...", (encrypt) ? "En" : "De");
    memset(info.ctr, 0x00, 16);
    memcpy(info.ctr, boss_header + 0x1C, 12);
    info.ctr[15] = 0x01;
    Debug("CTR: %08X%08X%08X%08X", getbe32(info.ctr), getbe32(info.ctr + 4), getbe32(info.ctr + 8), getbe32(info.ctr + 12));
    CryptSdToSd(filename, 0x28, fsize - 0x28, &info, false);
    
    // BOSS file verification (#2)
    if (!encrypt) {
        if (FileGetData(filename, content_header, 0x12, 0x28) != 0x12)
            return 1;
        if (FileGetData(filename, content_header_sha256, 0x20, 0x3A) != 0x20)
            return 1;
        sha_quick(l_sha256, content_header, 0x14, SHA256_MODE);
        if (memcmp(content_header_sha256, l_sha256, 0x20) == 0) {
            Debug("BOSS verification: OK");
        } else {
            Debug("BOSS verification: Failed");
            return 1;
        }
    }
    
    return 0;
}

u32 CryptGameFiles(u32 param)
{
    u8 ncch_crypt_none[8]     = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 };
    u8 ncch_crypt_standard[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    const u8 boss_magic[] = {0x62, 0x6F, 0x73, 0x73, 0x00, 0x01, 0x00, 0x01};
    const char* ncsd_partition_name[8] = {
        "Executable", "Manual", "DPC", "Unknown", "Unknown", "Unknown", "UpdateN3DS", "UpdateO3DS" 
    }; 
    const char* batch_dir = GetGameDir();
    u8* buffer = (u8*) 0x20316000;
    
    bool batch_ncch = param & GC_NCCH_PROCESS;
    bool batch_cia = param & GC_CIA_PROCESS;
    bool batch_boss = param & GC_BOSS_PROCESS;
    bool boss_encrypt = param & GC_BOSS_ENCRYPT;
    bool cia_encrypt = param & GC_CIA_ENCRYPT;
    bool cxi_only = param & GC_CXI_ONLY;
    u8* ncch_crypt = (param & GC_NCCH_ENCRYPT) ? ncch_crypt_standard : NULL;
    u8* cia_ncch_crypt = (param & GC_CIA_DEEP) ? ncch_crypt_none : ncch_crypt;
    
    u32 n_processed = 0;
    u32 n_failed = 0;
    
    if (!batch_dir || !DebugDirOpen(batch_dir)) {
        Debug("Game directory not found!");
        Debug("(check readme for more info)");
        return 1;
    }
    
    char path[256];
    u32 path_len = strnlen(batch_dir, 128);
    memcpy(path, batch_dir, path_len);
    path[path_len++] = '/';
    
    while (DirRead(path + path_len, 256 - path_len)) {
        if (FileGetData(path, buffer, 0x200, 0x0) != 0x200)
            continue;
        
        if (batch_ncch && (memcmp(buffer + 0x100, "NCCH", 4) == 0)) {
            Debug("Processing NCCH \"%s\"", path + path_len);
            if (CryptNcch(path, 0x00, 0, 0, ncch_crypt) != 1) {
                Debug("Success!");
                n_processed++;
            } else {
                Debug("Failed!");
                n_failed++;
            }
        } else if (batch_ncch && (memcmp(buffer + 0x100, "NCSD", 4) == 0)) {
            if (getle64(buffer + 0x110) != 0) 
                continue; // skip NAND backup NCSDs
            Debug("Processing NCSD \"%s\"", path + path_len);
            NcsdHeader* ncsd = (NcsdHeader*) buffer;
            u32 p;
            u32 nc = (cxi_only) ? 1 : 8;
            for (p = 0; p < nc; p++) {
                u64 seedId = (p) ? ncsd->mediaId : 0;
                u32 offset = ncsd->partitions[p].offset * 0x200;
                u32 size = ncsd->partitions[p].size * 0x200;
                if (size == 0) 
                    continue;
                Debug("Partition %i (%s)", p, ncsd_partition_name[p]);
                if (CryptNcch(path, offset, size, seedId, ncch_crypt) == 1)
                    break;
            }
            if ( p == nc ) {
                Debug("Success!");
                n_processed++;
            } else {
                Debug("Failed!");
                n_failed++;
            }
        } else if (batch_cia && (memcmp(buffer, "\x20\x20", 2) == 0)) {
            Debug("Processing CIA \"%s\"", path + path_len);
            if (CryptCia(path, cia_ncch_crypt, cia_encrypt, cxi_only) == 0) {
                Debug("Success!");
                n_processed++;
            } else {
                Debug("Failed!");
                n_failed++;
            }
        } else if (batch_boss && (memcmp(buffer, boss_magic, 8) == 0)) {
            Debug("Processing BOSS \"%s\"", path + path_len);
            if (CryptBoss(path, boss_encrypt) == 0) {
                Debug("Success!");
                n_processed++;
            } else {
                Debug("Failed!");
                n_failed++;
            }
        }
    }
    
    DirClose();
    
    if (n_processed) {
        Debug("");
        Debug("%ux processed / %ux failed ", n_processed, n_failed);
    } else if (!n_failed) {
        Debug("Nothing found in %s/!", batch_dir);
    }
    
    return !n_processed;
}

u32 CryptSdFiles(u32 param)
{
    (void) (param); // param is unused here
    const char* subpaths[] = {"dbs", "extdata", "title", NULL};
    const char* batch_dir = GetGameDir();
    u32 n_processed = 0;
    u32 n_failed = 0;
    u32 plen = 0;
    
    if (!batch_dir) {
        Debug("Game directory not found!");
        Debug("(check readme for more info)");
        return 1;
    }
    plen = strnlen(batch_dir, 128);
    
    // setup AES key from SD
    SetupSdKeyY0x34(false, NULL);
    
    // main processing loop
    for (u32 s = 0; subpaths[s] != NULL; s++) {
        char* filelist = (char*) 0x20400000;
        char basepath[128];
        u32 bplen;
        Debug("Processing subpath \"%s\"...", subpaths[s]);
        sprintf(basepath, "%s/%s", batch_dir, subpaths[s]);
        if (!GetFileList(basepath, filelist, 0x100000, true, true, false)) {
            Debug("Not found!");
            continue;
        }
        bplen = strnlen(basepath, 128);
        for (char* path = strtok(filelist, "\n"); path != NULL; path = strtok(NULL, "\n")) {
            u32 fsize = 0;
            CryptBufferInfo info = {.keyslot = 0x34, .setKeyY = 0, .mode = AES_CNT_CTRNAND_MODE};
            GetSdCtr(info.ctr, path + plen);
            if (FileOpen(path)) {
                fsize = FileGetSize();
                FileClose();
            } else {
                Debug("Could not open: %s", path + bplen);
                n_failed++;
                continue;
            }
            Debug("%2u: %s", n_processed, path + bplen);
            if (CryptSdToSd(path, 0, fsize, &info, true) == 0) {
                n_processed++;
            } else {
                Debug("Failed!");
                n_failed++;
            }
        }
    }
    
    return (n_processed) ? 0 : 1;
}

u32 DecryptSdFilesDirect(u32 param)
{
    (void) (param); // param is unused here
    char* filelist = (char*) 0x20400000;
    u8 movable_keyY[16] = { 0 };
    char basepath[256];
    const char* batch_dir = GetGameDir();
    u32 n_processed = 0;
    u32 n_failed = 0;
    u32 bplen = 0;
    
    if (!batch_dir) {
        Debug("Game directory not found!");
        Debug("(check readme for more info)");
        return 1;
    }
    
    if (SetupSdKeyY0x34(true, movable_keyY) != 0)
        return 1; // movable.sed has to be present in NAND
    
    Debug("");
    if (SdFolderSelector(basepath, movable_keyY) != 0)
        return 1;
    if (!GetFileList(basepath, filelist, 0x100000, true, true, false)) {
        Debug("Nothing found in folder");
        return 1;
    }
    Debug("");
    
    Debug("Using base path %s", basepath);
    bplen = strnlen(basepath, 256);
    
    // main processing loop
    for (char* srcpath = strtok(filelist, "\n"); srcpath != NULL; srcpath = strtok(NULL, "\n")) {
        char* subpath = srcpath + 13 + 33 + 33; // length of ("/Nintendo 3DS" + "/<id0>" + "/<id1>")
        char dstpath[256];
        u32 fsize = 0;
        snprintf(dstpath, 256, "%s%s", batch_dir, subpath);
        CryptBufferInfo info = {.keyslot = 0x34, .setKeyY = 0, .mode = AES_CNT_CTRNAND_MODE};
        GetSdCtr(info.ctr, subpath);
        Debug("%2u: %s", n_processed, srcpath + bplen);
        if (FileOpen(srcpath)) {
            fsize = FileGetSize();
            if (!DebugCheckFreeSpace(fsize))
                return 1;
            if (FileCopyTo(dstpath, BUFFER_ADDRESS, BUFFER_MAX_SIZE) != fsize) {
                Debug("Could not copy: %s", srcpath + bplen);
                n_failed++;
            }
            FileClose();
        } else {
            Debug("Could not open: %s", srcpath + bplen);
            n_failed++;
            continue;
        }
        if (CryptSdToSd(dstpath, 0, fsize, &info, true) == 0) {
            n_processed++;
        } else {
            Debug("Failed!");
            n_failed++;
        }
    }
    
    return (n_processed) ? 0 : 1;
}

static u32 DumpCartToFile(u32 offset, u32 size, u32 total, CryptBufferInfo* info, u8* out)
{
    // this assumes cart dumping initialized & file open for writing
    // also, careful, uses standard buffer
    u8* buffer = BUFFER_ADDRESS;
    u32 result = 0;
    
    if (info) {
        info->buffer = buffer;
    }
    
    // if offset does not start at sector boundary
    if (offset % 0x200) { 
        u32 read_bytes = 0x200 - (offset % 0x200);
        Cart_Dummy();
        Cart_Dummy();
        CTR_CmdReadData(offset / 0x200, 0x200, 1, buffer);
        memmove(buffer, buffer + (offset % 0x200), read_bytes);
        if (info) {
            info->size = read_bytes;
            CryptBuffer(info);
        }
        if (out) {
            memcpy(out, buffer, read_bytes);
            out += read_bytes;
        }
        if (!DebugFileWrite(buffer, read_bytes, offset)) 
           return 1;
        offset += read_bytes;
    }
    
    for (u64 i = 0; i < size; i += CART_CHUNK_SIZE) {
        u32 read_bytes = min(CART_CHUNK_SIZE, (size - i));
        if (total)
            ShowProgress(offset + i, total);
        Cart_Dummy();
        Cart_Dummy();
        CTR_CmdReadData((offset + i) / 0x200, 0x200, (read_bytes + 0x1FF) / 0x200, buffer);
        if (info) {
            info->size = read_bytes;
            CryptBuffer(info);
        }
        if (out) {
            memcpy(out, buffer, read_bytes);
            out += read_bytes;
        }
        if (!DebugFileWrite(buffer, read_bytes, offset + i)) {
            result = 1;
            break;
        }
    }
    ShowProgress(0, 0);
    
    return result;
}

static u32 DecryptCartNcchToFile(u32 offset, u32 size, u32 total)
{
    // this assumes cart dumping to be initialized already and file open for writing(!)
    // algorithm is simplified / slimmed when compared to CryptNcch()
    // and only has required capabilities
    NcchHeader* ncch = (NcchHeader*) 0x20317000;
    u8* exefs = (u8*) 0x20317200;
    CryptBufferInfo info = {.setKeyY = 0, .mode = AES_CNT_CTRNAND_MODE};
    u32 slot_base = 0x2C;
    u32 slot_7x = 0x2C;
    
    // read header
    Cart_Dummy();
    Cart_Dummy();
    CTR_CmdReadData(offset / 0x200, 0x200, 1, ncch);
    
    // check header, set up stuff
    if ((memcmp(ncch->magic, "NCCH", 4) != 0) || (ncch->size > (size / 0x200))) {
        Debug("Error reading partition NCCH header");
        return 1;
    }
    
    // check crypto, setup crypto
    if (ncch->flags[7] & 0x04) { // for unencrypted partitions...
        Debug("Not encrypted, dumping instead...");
        return DumpCartToFile(offset, size, total, NULL, NULL);
    } else if (ncch->flags[7] & 0x1) { // zeroKey / fixedKey crypto
        // from https://github.com/profi200/Project_CTR/blob/master/makerom/pki/dev.h
        u8 zeroKey[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        u8 sysKey[16]  = {0x52, 0x7C, 0xE6, 0x30, 0xA9, 0xCA, 0x30, 0x5F, 0x36, 0x96, 0xF3, 0xCD, 0xE9, 0x54, 0x19, 0x4B};
        slot_base = slot_7x = 0x11;
        setup_aeskey(0x11, (ncch->programId & ((u64) 0x10 << 32)) ? sysKey : zeroKey);
        use_aeskey(0x11);
    } else if (ncch->flags[3]) { // 7x crypto type
        slot_7x = (ncch->flags[3] == 0x0A) ? 0x18 : (ncch->flags[3] == 0x0B) ? 0x1B : 0x25;
        if (CheckKeySlot(slot_7x, 'X') != 0) {
            Debug("Slot0x%02XKeyX not set up", slot_7x);
            return 1;
        }
        setup_aeskeyY(slot_7x, ncch->signature);
        use_aeskey(slot_7x);
        setup_aeskeyY(slot_base, ncch->signature);
        use_aeskey(slot_base);
    } else { // standard crypto type (pre 7x)
        setup_aeskeyY(slot_base, ncch->signature);
        use_aeskey(slot_base);
    }
    
    // disable crypto in header, write to file
    ncch->flags[3] = 0x00;
    ncch->flags[7] &= (0x01|0x20)^0xFF;
    ncch->flags[7] |= 0x04;
    if (!DebugFileWrite(ncch, 0x200, offset))
        return 1;
        
    // process ExHeader
    if (ncch->size_exthdr > 0) {
        GetNcchCtr(info.ctr, ncch, 1);
        info.keyslot = slot_base;
        if (DumpCartToFile(offset + 0x200, 0x800, total, &info, NULL) != 0)
            return 1;
    }
    
    // logo region / plain region
    if (ncch->offset_exefs > 5) {
        if (DumpCartToFile(offset + 0xA00, (ncch->offset_exefs - 5) * 0x200, total, NULL, NULL) != 0)
            return 1;
    }
    
    // process ExeFS
    if (ncch->size_exefs > 0) {
        u32 offset_exefs = ncch->offset_exefs * 0x200;
        // include space between ExeFS and RomFS if possible
        u32 size_exefs = ((ncch->offset_romfs) ? (ncch->offset_romfs - ncch->offset_exefs) :
            ncch->size_exefs) * 0x200;
        // dump the whole thing encrypted, then overwrite with decrypted
        if (DumpCartToFile(offset + offset_exefs, size_exefs, total, NULL, NULL) != 0)
            return 1;
        // using 7x crypto routines for everything
        GetNcchCtr(info.ctr, ncch, 2);
        info.keyslot = slot_base;
        if (DumpCartToFile(offset + offset_exefs, 0x200, total, &info, exefs) != 0)
            return 1;
        for (u32 i = 0; i < 10; i++) {
            char* name = (char*) exefs + (i*0x10);
            u32 offset_exefs_file = getle32(exefs + (i*0x10) + 0x8) + 0x200;
            u32 size_exefs_file = getle32(exefs + (i*0x10) + 0xC);
            if (!size_exefs_file)
                continue;
            GetNcchCtr(info.ctr, ncch, 2);
            add_ctr(info.ctr, offset_exefs_file / 0x10);
            info.keyslot = ((strncmp(name, "banner", 8) == 0) || (strncmp(name, "icon", 8) == 0)) ? slot_base : slot_7x;
            if (DumpCartToFile(offset + offset_exefs + offset_exefs_file, size_exefs_file, total, &info, NULL) != 0)
                return 1;
        }
    }
    
    // process RomFS
    if (ncch->size_romfs > 0) {
        GetNcchCtr(info.ctr, ncch, 3);
        info.keyslot = slot_7x;
        if (DumpCartToFile(offset + (ncch->offset_romfs * 0x200), (ncch->size_romfs * 0x200), total, &info, NULL) != 0)
            return 1;
    }
    
    return 0;
}

u32 DumpGameCart(u32 param)
{
    NcsdHeader* ncsd = (NcsdHeader*) 0x20316000;
    NcchHeader* ncch = (NcchHeader*) 0x20317000;
    char filename[64];
    u64 cart_size = 0;
    u64 data_size = 0;
    u64 dump_size = 0;
    u32 result = 0;
    
    
    // check if cartridge inserted
    if (REG_CARDCONF2 & 0x1) {
        Debug("Cartridge was not detected");
        return 1;
    }
    
    // initialize cartridge
    Cart_Init();
    Debug("Cartridge ID: %08X", Cart_GetID());
    
    // read cartridge NCCH header
    CTR_CmdReadHeader(ncch);
    if (memcmp(ncch->magic, "NCCH", 4) != 0) {
        Debug("Error reading cart NCCH header");
        return 1;
    }

    // secure init
    u32 sec_keys[4];
    Cart_Secure_Init((u32*) ncch, sec_keys);
    
    // read NCSD header
    Cart_Dummy();
    CTR_CmdReadData(0, 0x200, 0x1000 / 0x200, ncsd);
    if (memcmp(ncsd->magic, "NCSD", 4) != 0) {
        Debug("Error reading cart NCSD header");
        return 1;
    }
    
    // check NCSD partition table
    cart_size = (u64) ncsd->size * 0x200;
    for (u32 i = 0; i < 8; i++) {
        NcchPartition* partition = ncsd->partitions + i;
        if ((partition->offset == 0) && (partition->size == 0))
            continue;
        if (partition->offset < (data_size / 0x200)) {
            Debug("Overlapping partitions in NCSD table");
            return 1; // should never happen
        }
        data_size = (u64) (partition->offset + partition->size) * 0x200;
    }
    
    // output some info
    Debug("Product ID: %.16s", ncch->productCode);
    Debug("Cartridge data size: %lluMB", cart_size / 0x100000);
    Debug("Cartridge used size: %lluMB", data_size / 0x100000);
    if (data_size > cart_size) {
        Debug("Used size exceeds cartridge size");
        return 1; // should never happen
    } else if ((data_size < 0x4000) || (cart_size < 0x4000)) {
        Debug("Bad cartridge size");
        return 1; // should never happen
    }
    dump_size = (param & CD_TRIM) ? data_size : cart_size;
    Debug("Cartridge dump size: %lluMB", dump_size / 0x100000);
    if ((dump_size == 0x100000000) && (data_size < dump_size)) {
        dump_size -= 0x200; // silently remove the last sector for 4GB ROMs
    } else if (dump_size >= 0x100000000) { // should not happen
        Debug("Error: Too big for the FAT32 file system");
        if (!(param & CD_TRIM))
            Debug("(maybe try dumping trimmed?)");
        return 1;
    }
    
    if (!DebugCheckFreeSpace((size_t) dump_size))
        return 1;
    
    // create file, write first 0x4000 byte
    Debug("");
    snprintf(filename, 64, "/%s%s%.16s%s.3ds", GetGameDir() ? GetGameDir() : "", GetGameDir() ? "/" : "", 
        ncch->productCode, (param & CD_DECRYPT) ? "-dec" : "");
    if (!FileCreate(filename, true)) {
        Debug("Could not create output file on SD");
        return 1;
    }
    memset(((u8*) ncsd) + 0x1200, 0xFF, 0x4000 - 0x1200);
    if (!DebugFileWrite((void*) ncsd, 0x4000, 0)) {
        FileClose();
        return 1;
    }
    
    if (!(param & CD_DECRYPT)) { // dump the encrypted cart
        Debug("Dumping cartridge %.16s (%lluMB)...", ncch->productCode, dump_size / 0x100000);
        result = DumpCartToFile(0x4000, dump_size - 0x4000, dump_size, NULL, NULL);
    } else { // dump decrypted partitions
        u32 p;
        for (p = 0; p < 8; p++) {
            u32 offset = ncsd->partitions[p].offset * 0x200;
            u32 size = ncsd->partitions[p].size * 0x200;
            if (size == 0) 
                continue;
            Debug("Decrypting partition #%lu (%luMB)...", p, size / 0x100000);
            if (DecryptCartNcchToFile(offset, size, dump_size) != 0)
                break;
        }
        if (p == 8) {
            Debug("Decryption success!");
        } else {
            Debug("Decryption failed!");
            result = 1;
        }
        
        if ((result == 0) && (dump_size > data_size)) {
            Debug("Dumping padding (%lluMB)...", (dump_size - data_size) / 0x100000);
            result = DumpCartToFile(data_size, dump_size - data_size, dump_size, NULL, NULL);
        }
    }
    FileClose();
    
    // verify decrypted ROM
    if ((result == 0) && (param & CD_DECRYPT)) {
        Debug("");
        for (u32 p = 0; p < 8; p++) {
            u32 offset = ncsd->partitions[p].offset * 0x200;
            u32 size = ncsd->partitions[p].size * 0x200;
            if (size == 0) 
                continue;
            Debug("Verifiying partition #%lu (%luMB)...", p, size / 0x100000);
            if (VerifyNcch(filename, offset) != 0)
                result = 1;
        }
        Debug("Verification %s", (result == 0) ? "success!" : "failed!");
    }
    
    
    return result;
}

u32 DumpPrivateHeader(u32 param)
{
    (void) param;
    NcchHeader* ncch = (NcchHeader*) 0x20317000;
    u8 privateHeader[0x50] = { 0xFF };
    u32 cartId = 0;
    char filename[64];
    
    
    // check if cartridge inserted
    if (REG_CARDCONF2 & 0x1) {
        Debug("Cartridge was not detected");
        return 1;
    }
    
    // initialize cartridge
    Cart_Init();
    cartId = Cart_GetID();
    Debug("Cartridge ID: %08X", cartId);
    *(u32*) (privateHeader + 0x40) = cartId;
    *(u32*) (privateHeader + 0x44) = 0x00000000;
    *(u32*) (privateHeader + 0x48) = 0xFFFFFFFF;
    *(u32*) (privateHeader + 0x4C) = 0xFFFFFFFF;
    
    // read cartridge NCCH header
    CTR_CmdReadHeader(ncch);
    if (memcmp(ncch->magic, "NCCH", 4) != 0) {
        Debug("Error reading cart NCCH header");
        return 1;
    }

    // secure init
    u32 sec_keys[4];
    Cart_Secure_Init((u32*) ncch, sec_keys);
    
    // get private header
    CTR_CmdReadUniqueID(privateHeader);
    Debug("Unique ID:");
    Debug("%016llX%016llX", getbe64(privateHeader), getbe64(privateHeader + 0x08));
    
    // dump to file
    snprintf(filename, 64, "/%s%s%.16s-private.bin", GetGameDir() ? GetGameDir() : "",
        GetGameDir() ? "/" : "", ncch->productCode);
    if (FileDumpData(filename, privateHeader, 0x50) != 0x50) {
        Debug("Could not create output file on SD");
        return 1;
    }
    
    return 0;
}
