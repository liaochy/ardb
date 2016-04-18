/*
 *Copyright (c) 2013-2013, yinqiwen <yinqiwen@gmail.com>
 *All rights reserved.
 *
 *Redistribution and use in source and binary forms, with or without
 *modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Redis nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 *THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 *BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "rdb.hpp"
#include "util/helpers.hpp"

extern "C"
{
#include "redis/lzf.h"
#include "redis/ziplist.h"
#include "redis/zipmap.h"
#include "redis/intset.h"
#include "redis/crc64.h"
#include "redis/endianconv.h"
}
#include <float.h>
#include <cmath>
#include <snappy.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "db/db.hpp"

#define RETURN_NEGATIVE_EXPR(x)  do\
    {                    \
	    int ret = (x);   \
        if(ret < 0) {   \
           return ret; \
        }             \
    }while(0)

#define REDIS_RDB_VERSION 6

#define ARDB_RDB_VERSION 1

/* Defines related to the dump file format. To store 32 bits lengths for short
 * keys requires a lot of space, so we check the most significant 2 bits of
 * the first byte to interpreter the length:
 *
 * 00|000000 => if the two MSB are 00 the len is the 6 bits of this byte
 * 01|000000 00000000 =>  01, the len is 14 byes, 6 bits + 8 bits of next byte
 * 10|000000 [32 bit integer] => if it's 01, a full 32 bit len will follow
 * 11|000000 this means: specially encoded object will follow. The six bits
 *           number specify the kind of object that follows.
 *           See the REDIS_RDB_ENC_* defines.
 *
 * Lengths up to 63 are stored using a single byte, most DB keys, and may
 * values, will fit inside. */
#define REDIS_RDB_6BITLEN 0
#define REDIS_RDB_14BITLEN 1
#define REDIS_RDB_32BITLEN 2
#define REDIS_RDB_ENCVAL 3
#define REDIS_RDB_LENERR UINT_MAX

/* When a length of a string object stored on disk has the first two bits
 * set, the remaining two bits specify a special encoding for the object
 * accordingly to the following defines: */
#define REDIS_RDB_ENC_INT8 0        /* 8 bit signed integer */
#define REDIS_RDB_ENC_INT16 1       /* 16 bit signed integer */
#define REDIS_RDB_ENC_INT32 2       /* 32 bit signed integer */
#define REDIS_RDB_ENC_LZF 3         /* string compressed with FASTLZ */

/* Special RDB opcodes (saved/loaded with rdbSaveType/rdbLoadType). */
#define REDIS_RDB_OPCODE_EXPIRETIME_MS 252
#define REDIS_RDB_OPCODE_EXPIRETIME 253
#define REDIS_RDB_OPCODE_SELECTDB   254
#define REDIS_RDB_OPCODE_EOF        255

/* Dup object types to RDB object types. Only reason is readability (are we
 * dealing with RDB types or with in-memory object types?). */
#define REDIS_RDB_TYPE_STRING 0
#define REDIS_RDB_TYPE_LIST   1
#define REDIS_RDB_TYPE_SET    2
#define REDIS_RDB_TYPE_ZSET   3
#define REDIS_RDB_TYPE_HASH   4

/* Object types for encoded objects. */
#define REDIS_RDB_TYPE_HASH_ZIPMAP    9
#define REDIS_RDB_TYPE_LIST_ZIPLIST  10
#define REDIS_RDB_TYPE_SET_INTSET    11
#define REDIS_RDB_TYPE_ZSET_ZIPLIST  12
#define REDIS_RDB_TYPE_HASH_ZIPLIST  13

#define ARDB_RDB_TYPE_CHUNK 1
#define ARDB_RDB_TYPE_SNAPPY_CHUNK 2
#define ARDB_RDB_TYPE_EOF 255

namespace ardb
{

    static const uint32 kloading_process_events_interval_bytes = 10 * 1024 * 1024;
    static const uint32 kmax_read_buffer_size = 10 * 1024 * 1024;
    DataDumpFile::DataDumpFile() :
            m_read_fp(NULL), m_write_fp(NULL), m_dump_ctx(NULL),m_db(NULL), m_cksm(0), m_routine_cb(
            NULL), m_routine_cbdata(
            NULL), m_processed_bytes(0), m_file_size(0), m_is_saving(false), m_last_save(0), m_routinetime(0), m_read_buf(
            NULL),m_expected_data_size(0),m_writed_data_size(0)
    {
        m_dump_ctx = new Context;
    }
    int DataDumpFile::Init(Ardb* db)
    {
        this->m_db = db;
        return 0;
    }

    void DataDumpFile::SetExpectedDataSize(int64 size)
    {
        m_expected_data_size = size;
    }

    int64 DataDumpFile::DumpLeftDataSize()
    {
        return m_expected_data_size - m_writed_data_size;
    }

    int64 DataDumpFile::ProcessLeftDataSize()
    {
        return m_expected_data_size - m_processed_bytes;
    }

    void DataDumpFile::Flush()
    {
        if (NULL != m_write_fp)
        {
            fflush(m_write_fp);
        }
    }

    int DataDumpFile::Rename(const std::string& default_file)
    {
        std::string file = m_db->GetConf().repl_data_dir;
        file.append("/").append(default_file);
        if(file == GetPath())
        {
            return 0;
        }
        Close();
        int ret = rename(GetPath().c_str(), file.c_str());
        if(0 == ret)
        {
            m_file_path = file;
        }
        return ret;
    }

    void DataDumpFile::Remove()
    {
        Close();
        unlink(m_file_path.c_str());
    }

    void DataDumpFile::Close()
    {
        if (NULL != m_read_fp)
        {
            fclose(m_read_fp);
            m_read_fp = NULL;
        }
        if (NULL != m_write_fp)
        {
            fclose(m_write_fp);
            m_write_fp = NULL;
        }
    }
    int DataDumpFile::OpenWriteFile(const std::string& file)
    {
        this->m_file_path = file;
        if ((m_write_fp = fopen(m_file_path.c_str(), "w")) == NULL)
        {
            ERROR_LOG("Failed to open ardb dump file:%s to write", m_file_path.c_str());
            return -1;
        }
        m_writed_data_size = 0;
        return 0;
    }

    int DataDumpFile::OpenReadFile(const std::string& file)
    {
        this->m_file_path = file;
        if ((m_read_fp = fopen(m_file_path.c_str(), "r")) == NULL)
        {
            ERROR_LOG("Failed to open ardb dump file:%s to write", m_file_path.c_str());
            return -1;
        }
        struct stat st;
        stat(m_file_path.c_str(), &st);
        m_file_size = st.st_size;
        if (NULL == m_read_buf)
        {
            NEW(m_read_buf, char[kmax_read_buffer_size]);
            setvbuf(m_read_fp, m_read_buf, _IOFBF, kmax_read_buffer_size);
        }
        return 0;
    }
    int DataDumpFile::Load(uint8 ctx_identity,const std::string& file, DumpRoutine* cb, void *data)
    {
//        m_dump_ctx->identity = ctx_identity;
        this->m_routine_cb = cb;
        this->m_routine_cbdata = data;
        int ret = OpenReadFile(file);
        if (0 != ret)
        {
            return ret;
        }
        return DoLoad();
    }

    int DataDumpFile::Write(const void* buf, size_t buflen)
    {
        /*
         * routine callback every 100ms
         */
        if (NULL != m_routine_cb && get_current_epoch_millis() - m_routinetime >= 100)
        {
            int cbret = m_routine_cb(m_routine_cbdata);
            if(0 != cbret)
            {
                return cbret;
            }
            m_routinetime = get_current_epoch_millis();
        }

        if (NULL == m_write_fp)
        {
            ERROR_LOG("Failed to open redis dump file:%s to write", m_file_path.c_str());
            return -1;
        }

        size_t max_write_bytes = 1024 * 1024 * 2;
        const char* data = (const char*) buf;
        while (buflen)
        {
            size_t bytes_to_write = (max_write_bytes < buflen) ? max_write_bytes : buflen;
            if (fwrite(data, bytes_to_write, 1, m_write_fp) == 0)
                return -1;
            //check sum here
            m_cksm = crc64(m_cksm, (unsigned char *) data, bytes_to_write);
            data += bytes_to_write;
            buflen -= bytes_to_write;
        }
        m_writed_data_size += buflen;
        return 0;
    }

    bool DataDumpFile::Read(void* buf, size_t buflen, bool cksm)
    {
        if ((m_processed_bytes + buflen) / kloading_process_events_interval_bytes
                > m_processed_bytes / kloading_process_events_interval_bytes)
        {
            INFO_LOG("%llu bytes loaded from dump file.", m_processed_bytes);
        }
        m_processed_bytes += buflen;
        /*
         * routine callback every 100ms
         */
        if (NULL != m_routine_cb && get_current_epoch_millis() - m_routinetime >= 100)
        {
            int cbret = m_routine_cb(m_routine_cbdata);
            if(0 != cbret)
            {
                return cbret;
            }
            m_routinetime = get_current_epoch_millis();
        }
        size_t max_read_bytes = 1024 * 1024 * 2;
        while (buflen)
        {
            size_t bytes_to_read = (max_read_bytes < buflen) ? max_read_bytes : buflen;
            if (fread(buf, bytes_to_read, 1, m_read_fp) == 0)
                return false;
            if (cksm)
            {
                //check sum here
                m_cksm = crc64(m_cksm, (unsigned char *) buf, bytes_to_read);
            }
            buf = (char*) buf + bytes_to_read;
            buflen -= bytes_to_read;
        }
        return true;
    }

    int DataDumpFile::Save(const std::string& file, DumpRoutine* cb, void *data)
    {
        if (m_is_saving)
        {
            return -1;
        }
        int ret = OpenWriteFile(file);
        if (0 != ret)
        {
            return ret;
        }
        m_is_saving = true;
        this->m_routine_cb = cb;
        this->m_routine_cbdata = data;
        ret = DoSave();
        Close();
        m_last_save = time(NULL);
        m_is_saving = false;
        return ret;
    }
    int DataDumpFile::BGSave(const std::string& file)
    {
        if (m_is_saving)
        {
            ERROR_LOG("There is already a background task saving data.");
            return -1;
        }
        struct BGTask: public Thread
        {
                DataDumpFile* serv;
                std::string path;
                BGTask(DataDumpFile* s, const std::string& file) :
                        serv(s), path(file)
                {
                }
                void Run()
                {
                    serv->Save(path, NULL, NULL);
                    delete this;
                }
        };
        BGTask* task = new BGTask(this, file);
        task->Start();
        return 0;
    }

    DataDumpFile::~DataDumpFile()
    {
        Close();
        DELETE_A(m_read_buf);
        DELETE(m_dump_ctx);
    }

    RedisDumpFile::RedisDumpFile()
    {
    }

    void RedisDumpFile::Close()
    {
        if (NULL != m_read_fp)
        {
            fclose(m_read_fp);
            m_read_fp = NULL;
        }
        if (NULL != m_write_fp)
        {
            fclose(m_write_fp);
            m_write_fp = NULL;
        }
    }

    int RedisDumpFile::ReadType()
    {
        unsigned char type;
        if (Read(&type, 1) == 0)
            return -1;
        return type;
    }

    time_t RedisDumpFile::ReadTime()
    {
        int32_t t32;
        if (Read(&t32, 4) == 0)
            return -1;
        return (time_t) t32;
    }
    int64 RedisDumpFile::ReadMillisecondTime()
    {
        int64_t t64;
        if (Read(&t64, 8) == 0)
            return -1;
        return (long long) t64;
    }

    uint32_t RedisDumpFile::ReadLen(int *isencoded)
    {
        unsigned char buf[2];
        uint32_t len;
        int type;

        if (isencoded)
            *isencoded = 0;
        if (Read(buf, 1) == 0)
            return REDIS_RDB_LENERR;
        type = (buf[0] & 0xC0) >> 6;
        if (type == REDIS_RDB_ENCVAL)
        {
            /* Read a 6 bit encoding type. */
            if (isencoded)
                *isencoded = 1;
            return buf[0] & 0x3F;
        }
        else if (type == REDIS_RDB_6BITLEN)
        {
            /* Read a 6 bit len. */
            return buf[0] & 0x3F;
        }
        else if (type == REDIS_RDB_14BITLEN)
        {
            /* Read a 14 bit len. */
            if (Read(buf + 1, 1) == 0)
                return REDIS_RDB_LENERR;
            return ((buf[0] & 0x3F) << 8) | buf[1];
        }
        else
        {
            /* Read a 32 bit len. */
            if (Read(&len, 4) == 0)
                return REDIS_RDB_LENERR;
            return ntohl(len);
        }
    }

    bool RedisDumpFile::ReadLzfStringObject(std::string& str)
    {
        unsigned int len, clen;
        unsigned char *c = NULL;

        if ((clen = ReadLen(NULL)) == REDIS_RDB_LENERR)
            return false;
        if ((len = ReadLen(NULL)) == REDIS_RDB_LENERR)
            return false;
        str.resize(len);
        NEW(c, unsigned char[clen]);
        if (NULL == c)
            return false;

        if (!Read(c, clen))
        {
            DELETE_A(c);
            return false;
        }
        str.resize(len);
        char* val = &str[0];
        if (lzf_decompress(c, clen, val, len) == 0)
        {
            DELETE_A(c);
            return false;
        }
        DELETE_A(c);
        return true;
    }

    bool RedisDumpFile::ReadInteger(int enctype, int64& val)
    {
        unsigned char enc[4];

        if (enctype == REDIS_RDB_ENC_INT8)
        {
            if (Read(enc, 1) == 0)
                return false;
            val = (signed char) enc[0];
        }
        else if (enctype == REDIS_RDB_ENC_INT16)
        {
            uint16_t v;
            if (Read(enc, 2) == 0)
                return false;
            v = enc[0] | (enc[1] << 8);
            val = (int16_t) v;
        }
        else if (enctype == REDIS_RDB_ENC_INT32)
        {
            uint32_t v;
            if (Read(enc, 4) == 0)
                return false;
            v = enc[0] | (enc[1] << 8) | (enc[2] << 16) | (enc[3] << 24);
            val = (int32_t) v;
        }
        else
        {
            val = 0; /* anti-warning */
            FATAL_LOG("Unknown RDB integer encoding type");
            abort();
        }
        return true;
    }

    /* For information about double serialization check rdbSaveDoubleValue() */
    int RedisDumpFile::ReadDoubleValue(double&val)
    {
        static double R_Zero = 0.0;
        static double R_PosInf = 1.0 / R_Zero;
        static double R_NegInf = -1.0 / R_Zero;
        static double R_Nan = R_Zero / R_Zero;
        char buf[128];
        unsigned char len;

        if (Read(&len, 1) == 0)
            return -1;
        switch (len)
        {
            case 255:
                val = R_NegInf;
                return 0;
            case 254:
                val = R_PosInf;
                return 0;
            case 253:
                val = R_Nan;
                return 0;
            default:
                if (Read(buf, len) == 0)
                    return -1;
                buf[len] = '\0';
                sscanf(buf, "%lg", &val);
                return 0;
        }
    }

    bool RedisDumpFile::ReadString(std::string& str)
    {
        str.clear();
        int isencoded;
        uint32_t len;
        len = ReadLen(&isencoded);
        if (isencoded)
        {
            switch (len)
            {
                case REDIS_RDB_ENC_INT8:
                case REDIS_RDB_ENC_INT16:
                case REDIS_RDB_ENC_INT32:
                {
                    int64 v;
                    if (ReadInteger(len, v))
                    {
                        str = stringfromll(v);
                        return true;
                    }
                    else
                    {
                        return true;
                    }
                }
                case REDIS_RDB_ENC_LZF:
                {
                    return ReadLzfStringObject(str);
                }
                default:
                {
                    ERROR_LOG("Unknown RDB encoding type");
                    abort();
                    break;
                }
            }
        }

        if (len == REDIS_RDB_LENERR)
            return false;
        str.clear();
        str.resize(len);
        char* val = &str[0];
        if (len && Read(val, len) == 0)
        {
            return false;
        }
        return true;
    }

    void RedisDumpFile::LoadListZipList(unsigned char* data, const std::string& key)
    {
        unsigned char* iter = ziplistIndex(data, 0);
        Context& tmpctx;
        ValueObject listmeta;
        listmeta.SetType(KEY_LIST);
        //BatchWriteGuard guard(m_db->GetKeyValueEngine());
        int64 idx = 0;
        while (iter != NULL)
        {
            unsigned char *vstr;
            unsigned int vlen;
            long long vlong;
            if (ziplistGet(iter, &vstr, &vlen, &vlong))
            {
                std::string value;
                if (vstr)
                {
                    value.assign((char*) vstr, vlen);
                }
                else
                {
                    value = stringfromll(vlong);
                }
                KeyObject lk(m_current_db, KEY_LIST_ELEMENT, key);
                lk.SetListIndex(idx);
                ValueObject lv;
                lv.SetType(KEY_LIST_ELEMENT);
                lv.SetListElement(value);
                //g_db->Set
                idx++;
                //m_db->ListInsert(tmpctx, listmeta, NULL, value, true, false);
            }
            iter = ziplistNext(data, iter);
        }
        listmeta.SetObjectLen(idx);
        listmeta.SetListMinIdx(0);
        listmeta.SetListMaxIdx(idx - 1);
        listmeta.GetListMeta().sequential = true;
        //m_db->SetKeyValue(tmpctx, listmeta);
    }

    static double zzlGetScore(unsigned char *sptr)
    {
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        char buf[128];
        double score;

        ASSERT(sptr != NULL);
        ASSERT(ziplistGet(sptr, &vstr, &vlen, &vlong));
        if (vstr)
        {
            memcpy(buf, vstr, vlen);
            buf[vlen] = '\0';
            score = strtod(buf, NULL);
        }
        else
        {
            score = vlong;
        }
        return score;
    }
    void RedisDumpFile::LoadZSetZipList(unsigned char* data, const std::string& key)
    {
        Context& tmpctx = *m_dump_ctx;
        ValueObject zmeta;
        tmpctx.currentDB = m_current_db;
        zmeta.key.db = m_current_db;
        zmeta.key.key = key;
        zmeta.key.type = KEY_META;
        zmeta.type = ZSET_META;
        zmeta.meta.SetEncoding(COLLECTION_ENCODING_ZIPZSET);
        //BatchWriteGuard guard(m_db->GetKeyValueEngine());
        unsigned char* iter = ziplistIndex(data, 0);
        while (iter != NULL)
        {
            unsigned char *vstr;
            unsigned int vlen;
            long long vlong;
            std::string value;
            if (ziplistGet(iter, &vstr, &vlen, &vlong))
            {
                if (vstr)
                {
                    value.assign((char*) vstr, vlen);
                }
                else
                {
                    value = stringfromll(vlong);
                }
            }
            iter = ziplistNext(data, iter);
            if (NULL == iter)
            {
                break;
            }
            double score = zzlGetScore(iter);
            Data element, scorev;
            element.SetString(value, true);
            scorev.SetDouble(score);
            m_db->ZSetAdd(tmpctx, zmeta, element, scorev, NULL);
            iter = ziplistNext(data, iter);
        }
        m_db->SetKeyValue(tmpctx, zmeta);
    }

    void RedisDumpFile::LoadHashZipList(unsigned char* data, const std::string& key)
    {
        Context& tmpctx = *m_dump_ctx;
        ValueObject zmeta;
        tmpctx.currentDB = m_current_db;
        zmeta.key.db = m_current_db;
        zmeta.key.type = KEY_META;
        zmeta.key.key = key;
        zmeta.type = HASH_META;
        zmeta.meta.SetEncoding(COLLECTION_ENCODING_ZIPMAP);
        //BatchWriteGuard guard(m_db->GetKeyValueEngine());
        unsigned char* iter = ziplistIndex(data, 0);
        while (iter != NULL)
        {
            unsigned char *vstr, *fstr;
            unsigned int vlen, flen;
            long long vlong, flong;
            std::string field, value;
            if (ziplistGet(iter, &fstr, &flen, &flong))
            {
                if (fstr)
                {
                    field.assign((char*) fstr, flen);
                }
                else
                {
                    field = stringfromll(flong);
                }
            }
            else
            {
                break;
            }
            iter = ziplistNext(data, iter);
            if (NULL == iter)
            {
                break;
            }
            if (ziplistGet(iter, &vstr, &vlen, &vlong))
            {
                if (vstr)
                {
                    value.assign((char*) vstr, vlen);
                }
                else
                {
                    value = stringfromll(vlong);
                }
                Data f, v;
                f.SetString(field, true);
                v.SetString(value, true);
                m_db->HashSet(tmpctx, zmeta, f, v);
                //m_db->HSet(m_current_db, key, field, value);
            }
            iter = ziplistNext(data, iter);
        }
        m_db->SetKeyValue(tmpctx, zmeta);
    }

    void RedisDumpFile::LoadSetIntSet(unsigned char* data, const std::string& key)
    {
        int ii = 0;
        int64_t llele = 0;
        Context& tmpctx = *m_dump_ctx;
        ValueObject zmeta;
        tmpctx.currentDB = m_current_db;
        zmeta.key.db = m_current_db;
        zmeta.key.type = KEY_META;
        zmeta.key.key = key;
        zmeta.type = SET_META;
        zmeta.meta.SetEncoding(COLLECTION_ENCODING_ZIPSET);
        while (!intsetGet((intset*) data, ii++, &llele))
        {
            //value
            //m_db->SAdd(m_current_db, key, stringfromll(llele));
            bool tmp;
            m_db->SetAdd(tmpctx, zmeta, stringfromll(llele), tmp);
        }
        m_db->SetKeyValue(tmpctx, zmeta);
    }

    bool RedisDumpFile::LoadObject(int rdbtype, const std::string& key)
    {
        Context& tmpctx = *m_dump_ctx;
        tmpctx.currentDB = m_current_db;
        BatchWriteGuard guard(tmpctx);
        switch (rdbtype)
        {
            case REDIS_RDB_TYPE_STRING:
            {
                std::string str;
                if (ReadString(str))
                {
                    //save key-value
                    GenericSetOptions options;
                    options.fill_reply = false;
                    m_db->GenericSet(tmpctx, key, str, options);
                }
                else
                {
                    return false;
                }
                break;
            }
            case REDIS_RDB_TYPE_LIST:
            case REDIS_RDB_TYPE_SET:
            {
                uint32 len;
                if ((len = ReadLen(NULL)) == REDIS_RDB_LENERR)
                    return false;
                ValueObject zmeta;
                tmpctx.currentDB = m_current_db;
                zmeta.key.db = m_current_db;
                zmeta.key.type = KEY_META;
                zmeta.key.key = key;
                if (REDIS_RDB_TYPE_SET == rdbtype)
                {
                    zmeta.type = SET_META;
                    zmeta.meta.SetEncoding(COLLECTION_ENCODING_ZIPSET);
                }
                else
                {
                    zmeta.type = LIST_META;
                    zmeta.meta.SetEncoding(COLLECTION_ENCODING_ZIPLIST);
                }
                while (len--)
                {
                    std::string str;
                    if (ReadString(str))
                    {
                        //push to list/set
                        if (REDIS_RDB_TYPE_SET == rdbtype)
                        {
                            bool tmp;
                            m_db->SetAdd(tmpctx, zmeta, str, tmp);
                        }
                        else
                        {
                            m_db->ListInsert(tmpctx, zmeta, NULL, str, true, false);
                        }
                    }
                    else
                    {
                        return false;
                    }
                }
                m_db->SetKeyValue(tmpctx, zmeta);
                break;
            }
            case REDIS_RDB_TYPE_ZSET:
            {
                uint32 len;
                if ((len = ReadLen(NULL)) == REDIS_RDB_LENERR)
                    return false;
                ValueObject zmeta;
                zmeta.key.db = m_current_db;
                zmeta.key.type = KEY_META;
                zmeta.type = ZSET_META;
                zmeta.key.key = key;
                zmeta.meta.SetEncoding(COLLECTION_ENCODING_ZIPZSET);
                while (len--)
                {
                    std::string str;
                    double score;
                    if (ReadString(str) && 0 == ReadDoubleValue(score))
                    {
                        //save value score
                        //m_db->ZAdd(m_current_db, key, score, str);
                        Data element, scorev;
                        element.SetString(str, true);
                        scorev.SetDouble(score);
                        m_db->ZSetAdd(tmpctx, zmeta, element, scorev, NULL);
                    }
                    else
                    {
                        return false;
                    }
                }
                m_db->SetKeyValue(tmpctx, zmeta);
                break;
            }
            case REDIS_RDB_TYPE_HASH:
            {
                uint32 len;
                if ((len = ReadLen(NULL)) == REDIS_RDB_LENERR)
                    return false;
                ValueObject zmeta;
                zmeta.key.db = m_current_db;
                zmeta.key.type = KEY_META;
                zmeta.type = HASH_META;
                zmeta.key.key = key;
                zmeta.meta.SetEncoding(COLLECTION_ENCODING_ZIPMAP);
                while (len--)
                {
                    std::string field, str;
                    if (ReadString(field) && ReadString(str))
                    {
                        //save hash value
                        Data f, v;
                        f.SetString(field, true);
                        v.SetString(str, true);
                        m_db->HashSet(tmpctx, zmeta, f, v);
                    }
                    else
                    {
                        return false;
                    }
                }
                m_db->SetKeyValue(tmpctx, zmeta);
                break;
            }
            case REDIS_RDB_TYPE_HASH_ZIPMAP:
            case REDIS_RDB_TYPE_LIST_ZIPLIST:
            case REDIS_RDB_TYPE_SET_INTSET:
            case REDIS_RDB_TYPE_ZSET_ZIPLIST:
            case REDIS_RDB_TYPE_HASH_ZIPLIST:
            {
                std::string aux;
                if (!ReadString(aux))
                {
                    return false;
                }
                unsigned char* data = (unsigned char*) (&(aux[0]));
                switch (rdbtype)
                {
                    case REDIS_RDB_TYPE_HASH_ZIPMAP:
                    {
                        unsigned char *zi = zipmapRewind(data);
                        unsigned char *fstr, *vstr;
                        unsigned int flen, vlen;
                        unsigned int maxlen = 0;
                        ValueObject zmeta;
                        zmeta.key.db = m_current_db;
                        zmeta.key.type = KEY_META;
                        zmeta.type = HASH_META;
                        zmeta.key.key = key;
                        zmeta.meta.SetEncoding(COLLECTION_ENCODING_ZIPMAP);
                        while ((zi = zipmapNext(zi, &fstr, &flen, &vstr, &vlen)) != NULL)
                        {
                            if (flen > maxlen)
                                maxlen = flen;
                            if (vlen > maxlen)
                                maxlen = vlen;

                            //save hash value data
                            Slice field((char*) fstr, flen);
                            Slice value((char*) vstr, vlen);
                            //m_db->HSet(m_current_db, key, field, value);
                            Data f, v;
                            f.SetString(field, true);
                            v.SetString(value, true);
                            m_db->HashSet(tmpctx, zmeta, f, v);
                        }
                        break;
                    }
                    case REDIS_RDB_TYPE_LIST_ZIPLIST:
                    {
                        LoadListZipList(data, key);
                        break;
                    }
                    case REDIS_RDB_TYPE_SET_INTSET:
                    {
                        LoadSetIntSet(data, key);
                        break;
                    }
                    case REDIS_RDB_TYPE_ZSET_ZIPLIST:
                    {
                        LoadZSetZipList(data, key);
                        break;
                    }
                    case REDIS_RDB_TYPE_HASH_ZIPLIST:
                    {
                        LoadHashZipList(data, key);
                        break;
                    }
                    default:
                    {
                        ERROR_LOG("Unknown encoding");
                        abort();
                    }
                }
                break;
            }
            default:
            {
                ERROR_LOG("Unknown object type:%d", rdbtype);
                abort();
            }
        }
        return true;
    }

    int RedisDumpFile::IsRedisDumpFile(const std::string& file)
    {
        FILE* fp = NULL;
        if ((fp = fopen(file.c_str(), "r")) == NULL)
        {
            ERROR_LOG("Failed to load redis dump file:%s", file.c_str());
            return -1;
        }
        char buf[10];
        if (fread(buf, 10, 1, fp) != 1)
        {
            fclose(fp);
            return -1;
        }
        buf[9] = '\0';
        fclose(fp);
        if (memcmp(buf, "REDIS", 5) != 0)
        {
            WARN_LOG("Wrong signature trying to load DB from file:%s", file.c_str());
            return 0;
        }
        return 1;
    }

    int RedisDumpFile::DoLoad()
    {
        char buf[1024];
        int rdbver, type;
        int64 expiretime = -1;
        std::string key;

        m_current_db = 0;
        Context& tmpctx = *m_dump_ctx;
        BatchWriteGuard guard(tmpctx);
        if (!Read(buf, 9, true))
            goto eoferr;
        buf[9] = '\0';
        if (memcmp(buf, "REDIS", 5) != 0)
        {
            Close();
            WARN_LOG("Wrong signature:%s trying to load DB from file:%s", buf, m_file_path.c_str());
            return -1;
        }
        rdbver = atoi(buf + 5);
        if (rdbver < 1 || rdbver > REDIS_RDB_VERSION)
        {
            WARN_LOG("Can't handle RDB format version %d", rdbver);
            return -1;
        }

        while (true)
        {
            expiretime = -1;
            /* Read type. */
            if ((type = ReadType()) == -1)
                goto eoferr;
            if (type == REDIS_RDB_OPCODE_EXPIRETIME)
            {
                if ((expiretime = ReadTime()) == -1)
                    goto eoferr;
                /* We read the time so we need to read the object type again. */
                if ((type = ReadType()) == -1)
                    goto eoferr;
                /* the EXPIRETIME opcode specifies time in seconds, so convert
                 * into milliseconds. */
                expiretime *= 1000;
            }
            else if (type == REDIS_RDB_OPCODE_EXPIRETIME_MS)
            {
                /* Milliseconds precision expire times introduced with RDB
                 * version 3. */
                if ((expiretime = ReadMillisecondTime()) == -1)
                    goto eoferr;
                /* We read the time so we need to read the object type again. */
                if ((type = ReadType()) == -1)
                    goto eoferr;
            }

            if (type == REDIS_RDB_OPCODE_EOF)
                break;

            /* Handle SELECT DB opcode as a special case */
            if (type == REDIS_RDB_OPCODE_SELECTDB)
            {
                if ((m_current_db = ReadLen(NULL)) == REDIS_RDB_LENERR)
                {
                    ERROR_LOG("Failed to read current DBID.");
                    goto eoferr;
                }
                continue;
            }
            //load key, object

            if (!ReadString(key))
            {
                ERROR_LOG("Failed to read current key.");
                goto eoferr;
            }
            Context& tmpctx = *m_dump_ctx;
            tmpctx.currentDB = m_current_db;
            m_db->DeleteKey(tmpctx, key);
            if (!LoadObject(type, key))
            {
                ERROR_LOG("Failed to load object:%d", type);
                goto eoferr;
            }
            if (-1 != expiretime)
            {
                m_db->GenericExpire(tmpctx, key, expiretime);
                //m_db->Pexpireat(m_current_db, key, expiretime);
            }
        }

        /* Verify the checksum if RDB version is >= 5 */
        if (rdbver >= 5)
        {
            uint64_t cksum, expected = m_cksm;
            if (!Read(&cksum, 8))
            {
                goto eoferr;
            }memrev64ifbe(&cksum);
            if (cksum == 0)
            {
                WARN_LOG("RDB file was saved with checksum disabled: no check performed.");
            }
            else if (cksum != expected)
            {
                ERROR_LOG("Wrong RDB checksum. Aborting now(%llu-%llu)", cksum, expected);
                exit(1);
            }
        }
        Close();
        INFO_LOG("Redis dump file load finished.");
        return 0;
        eoferr: Close();
        WARN_LOG("Short read or OOM loading DB. Unrecoverable error, aborting now.");
        return -1;
    }

    void RedisDumpFile::WriteMagicHeader()
    {
        char magic[10];
        snprintf(magic, sizeof(magic), "REDIS%04d", REDIS_RDB_VERSION);
        Write(magic, 9);
    }

    int RedisDumpFile::WriteType(uint8 type)
    {
        return Write(&type, 1);
    }

    int RedisDumpFile::WriteKeyType(KeyType type)
    {
        switch (type)
        {
            case KEY_STRING:
            {
                return WriteType(REDIS_RDB_TYPE_STRING);
            }
            case KEY_SET:
            case KEY_SET_MEMBER:
            {
                return WriteType(REDIS_RDB_TYPE_SET);
            }
            case KEY_LIST:
            case KEY_LIST_ELEMENT:
            {
                return WriteType(REDIS_RDB_TYPE_LIST);
            }
            case KEY_ZSET:
            case KEY_ZSET_SCORE:
            case KEY_ZSET_SORT:
            {
                return WriteType(REDIS_RDB_TYPE_ZSET);
            }
            case KEY_HASH:
            case KEY_HASH_FIELD:
            {
                return WriteType(REDIS_RDB_TYPE_HASH);
            }
            default:
            {
                return -1;
            }
        }
    }

    int RedisDumpFile::WriteDouble(double val)
    {
        unsigned char buf[128];
        int len;

        if (std::isnan(val))
        {
            buf[0] = 253;
            len = 1;
        }
        else if (!std::isfinite(val))
        {
            len = 1;
            buf[0] = (val < 0) ? 255 : 254;
        }
        else
        {
#if (DBL_MANT_DIG >= 52) && (LLONG_MAX == 0x7fffffffffffffffLL)
            /* Check if the float is in a safe range to be casted into a
             * long long. We are assuming that long long is 64 bit here.
             * Also we are assuming that there are no implementations around where
             * double has precision < 52 bit.
             *
             * Under this assumptions we test if a double is inside an interval
             * where casting to long long is safe. Then using two castings we
             * make sure the decimal part is zero. If all this is true we use
             * integer printing function that is much faster. */
            double min = -4503599627370495LL; /* (2^52)-1 */
            double max = 4503599627370496LL; /* -(2^52) */
            if (val > min && val < max && val == ((double) ((long long) val)))
            {
                ll2string((char*) buf + 1, sizeof(buf), (long long) val);
            }
            else
#endif
                snprintf((char*) buf + 1, sizeof(buf) - 1, "%.17g", val);
            buf[0] = strlen((char*) buf + 1);
            len = buf[0] + 1;
        }
        return Write(buf, len);

    }

    int RedisDumpFile::WriteMillisecondTime(uint64 ts)
    {
        return Write(&ts, 8);
    }

    int RedisDumpFile::WriteTime(time_t t)
    {
        int32_t t32 = (int32_t) t;
        return Write(&t32, 4);
    }

    static int EncodeInteger(long long value, unsigned char *enc)
    {
        /* Finally check if it fits in our ranges */
        if (value >= -(1 << 7) && value <= (1 << 7) - 1)
        {
            enc[0] = (REDIS_RDB_ENCVAL << 6) | REDIS_RDB_ENC_INT8;
            enc[1] = value & 0xFF;
            return 2;
        }
        else if (value >= -(1 << 15) && value <= (1 << 15) - 1)
        {
            enc[0] = (REDIS_RDB_ENCVAL << 6) | REDIS_RDB_ENC_INT16;
            enc[1] = value & 0xFF;
            enc[2] = (value >> 8) & 0xFF;
            return 3;
        }
        else if (value >= -((long long) 1 << 31) && value <= ((long long) 1 << 31) - 1)
        {
            enc[0] = (REDIS_RDB_ENCVAL << 6) | REDIS_RDB_ENC_INT32;
            enc[1] = value & 0xFF;
            enc[2] = (value >> 8) & 0xFF;
            enc[3] = (value >> 16) & 0xFF;
            enc[4] = (value >> 24) & 0xFF;
            return 5;
        }
        else
        {
            return 0;
        }
    }

    /* String objects in the form "2391" "-100" without any space and with a
     * range of values that can fit in an 8, 16 or 32 bit signed value can be
     * encoded as integers to save space */
    static int TryIntegerEncoding(char *s, size_t len, unsigned char *enc)
    {
        long long value;
        char *endptr, buf[32];

        /* Check if it's possible to encode this value as a number */
        value = strtoll(s, &endptr, 10);
        if (endptr[0] != '\0')
            return 0;
        ll2string(buf, 32, value);

        /* If the number converted back into a string is not identical
         * then it's not possible to encode the string as integer */
        if (strlen(buf) != len || memcmp(buf, s, len))
            return 0;

        return EncodeInteger(value, enc);
    }

    /* Like rdbSaveStringObjectRaw() but handle encoded objects */
    int RedisDumpFile::WriteStringObject(Data& o)
    {
        /* Avoid to decode the object, then encode it again, if the
         * object is alrady integer encoded. */
        if (o.IsInteger())
        {
            return WriteLongLongAsStringObject(o.GetInt64());
        }
        else
        {
            o.ToString();
            return WriteRawString(o.RawString(), sdslen(o.RawString()));
        }
    }

    /* Save a string objet as [len][data] on disk. If the object is a string
     * representation of an integer value we try to save it in a special form */
    int RedisDumpFile::WriteRawString(const char *s, size_t len)
    {
        int enclen;
        int n, nwritten = 0;

        /* Try integer encoding */
        if (len > 0 && len <= 11)
        {
            unsigned char buf[5];
            if ((enclen = TryIntegerEncoding((char*) s, len, buf)) > 0)
            {
                if (Write(buf, enclen) == -1)
                    return -1;
                return enclen;
            }
        }

        /* Try LZF compression - under 20 bytes it's unable to compress even
         * aaaaaaaaaaaaaaaaaa so skip it */
        if (len > 20)
        {
            n = WriteLzfStringObject(s, len);
            if (n == -1)
                return -1;
            if (n > 0)
                return n;
            /* Return value of 0 means data can't be compressed, save the old way */
        }

        /* Store verbatim */
        if ((n = WriteLen(len)) == -1)
            return -1;
        nwritten += n;
        if (len > 0)
        {
            if (Write(s, len) == -1)
                return -1;
            nwritten += len;
        }
        return nwritten;
    }

    int RedisDumpFile::WriteLzfStringObject(const char *s, size_t len)
    {
        size_t comprlen, outlen;
        unsigned char byte;
        int n, nwritten = 0;
        void *out;

        /* We require at least four bytes compression for this to be worth it */
        if (len <= 4)
            return 0;
        outlen = len - 4;
        if ((out = malloc(outlen + 1)) == NULL)
            return 0;
        comprlen = lzf_compress(s, len, out, outlen);
        if (comprlen == 0)
        {
            free(out);
            return 0;
        }
        /* Data compressed! Let's save it on disk */
        byte = (REDIS_RDB_ENCVAL << 6) | REDIS_RDB_ENC_LZF;
        if ((n = Write(&byte, 1)) == -1)
            goto writeerr;
        nwritten += n;

        if ((n = WriteLen(comprlen)) == -1)
            goto writeerr;
        nwritten += n;

        if ((n = WriteLen(len)) == -1)
            goto writeerr;
        nwritten += n;

        if ((n = Write(out, comprlen)) == -1)
            goto writeerr;
        nwritten += n;

        free(out);
        return nwritten;

        writeerr: free(out);
        return -1;
    }

    /* Save a long long value as either an encoded string or a string. */
    int RedisDumpFile::WriteLongLongAsStringObject(long long value)
    {
        unsigned char buf[32];
        int n, nwritten = 0;
        int enclen = EncodeInteger(value, buf);
        if (enclen > 0)
        {
            return Write(buf, enclen);
        }
        else
        {
            /* Encode as string */
            enclen = ll2string((char*) buf, 32, value);
            if ((n = WriteLen(enclen)) == -1)
                return -1;
            nwritten += n;
            if ((n = Write(buf, enclen)) == -1)
                return -1;
            nwritten += n;
        }
        return nwritten;
    }

    int RedisDumpFile::WriteLen(uint32 len)
    {
        unsigned char buf[2];
        size_t nwritten;

        if (len < (1 << 6))
        {
            /* Save a 6 bit len */
            buf[0] = (len & 0xFF) | (REDIS_RDB_6BITLEN << 6);
            if (Write(buf, 1) == -1)
                return -1;
            nwritten = 1;
        }
        else if (len < (1 << 14))
        {
            /* Save a 14 bit len */
            buf[0] = ((len >> 8) & 0xFF) | (REDIS_RDB_14BITLEN << 6);
            buf[1] = len & 0xFF;
            if (Write(buf, 2) == -1)
                return -1;
            nwritten = 2;
        }
        else
        {
            /* Save a 32 bit len */
            buf[0] = (REDIS_RDB_32BITLEN << 6);
            if (Write(buf, 1) == -1)
                return -1;
            len = htonl(len);
            if (Write(&len, 4) == -1)
                return -1;
            nwritten = 1 + 4;
        }
        return nwritten;
    }

    int RedisDumpFile::DoSave()
    {
        WriteMagicHeader();

#define DUMP_CHECK_WRITE(x)  if((err = (x)) < 0) break
        KeyObject k;
        k.db = 0;
        k.type = KEY_META;
        Context& tmpctx = *m_dump_ctx;
        BatchWriteGuard guard(tmpctx);
        Iterator* iter = m_db->IteratorKeyValue(k, false);
        uint32 cursor = 0;
        int err = 0;
        if (NULL != iter)
        {
            std::string currentKey;
            while (iter->Valid())
            {
                KeyObject kk;
                ValueObject vv;
                if (!decode_key(iter->Key(), kk) || !decode_value(iter->Value(), vv))
                {
                    ERROR_LOG("Failed to decode key or value.");
                    break;
                }
                if (kk.db == ARDB_GLOBAL_DB)
                {
                    return -1;
                }
                if (cursor == 0 || (kk.db != m_current_db))
                {
                    m_current_db = kk.db;
                    tmpctx.currentDB = kk.db;
                    currentKey.clear();
                    DUMP_CHECK_WRITE(WriteType(REDIS_RDB_OPCODE_SELECTDB));
                    DUMP_CHECK_WRITE(WriteLen(m_current_db));
                }
                cursor++;
                if (kk.type != KEY_META && kk.type != LIST_ELEMENT && kk.type != ZSET_ELEMENT_SCORE
                        && kk.type != SET_ELEMENT && kk.type != BITSET_ELEMENT && kk.type != HASH_FIELD)
                {
                    iter->Next();
                    continue;
                }
                if (kk.type == KEY_META)
                {
                    kk.type = vv.type;
                    switch (vv.type)
                    {
                        case STRING_META:
                        {
                            kk.type = STRING_META;
                            break;
                        }
                        case LIST_META:
                        case HASH_META:
                        case ZSET_META:
                        case SET_META:
                        {
                            if (vv.meta.Encoding() == COLLECTION_ENCODING_RAW)
                            {
                                iter->Next();
                                continue;
                            }
                            break;
                        }
                        case BITSET_META:
                        default:
                        {
                            iter->Next();
                            continue;
                        }
                    }
                }

                bool firstElementInKey = false;
                if (currentKey.size() != kk.key.size() || strncmp(currentKey.c_str(), kk.key.data(), currentKey.size()))
                {
                    uint64 expiretime = 0;
                    m_db->GenericTTL(tmpctx, kk.key, expiretime);
                    if (expiretime > 0)
                    {
                        DUMP_CHECK_WRITE(WriteType(REDIS_RDB_OPCODE_EXPIRETIME_MS));
                        DUMP_CHECK_WRITE(WriteMillisecondTime(expiretime));
                    }
                    DUMP_CHECK_WRITE(WriteKeyType((KeyType )kk.type));
                    currentKey.assign(kk.key.data(), kk.key.size());
                    DUMP_CHECK_WRITE(WriteRawString(currentKey.data(), currentKey.size()));
                    firstElementInKey = true;
                }

                switch (kk.type)
                {
                    case STRING_META:
                    {
                        DUMP_CHECK_WRITE(WriteStringObject(vv.meta.str_value));
                        break;
                    }
                    case LIST_ELEMENT:
                    {
                        if (firstElementInKey)
                        {
                            m_db->ListLen(tmpctx, kk.key);
                            DUMP_CHECK_WRITE(WriteLen(tmpctx.reply.integer));
                        }
                        DUMP_CHECK_WRITE(WriteStringObject(vv.element));
                        break;
                    }
                    case SET_ELEMENT:
                    {
                        if (firstElementInKey)
                        {
                            m_db->SetLen(tmpctx, kk.key);
                            DUMP_CHECK_WRITE(WriteLen(tmpctx.reply.integer));
                        }
                        DUMP_CHECK_WRITE(WriteStringObject(kk.element));
                        break;
                    }
                    case ZSET_ELEMENT_SCORE:
                    {
                        if (firstElementInKey)
                        {
                            m_db->ZSetLen(tmpctx, kk.key);
                            DUMP_CHECK_WRITE(WriteLen(tmpctx.reply.integer));
                        }
                        DUMP_CHECK_WRITE(WriteStringObject(kk.element));
                        DUMP_CHECK_WRITE(WriteDouble(kk.score.NumberValue()));
                        break;
                    }
                    case HASH_FIELD:
                    {
                        if (firstElementInKey)
                        {
                            m_db->HashLen(tmpctx, kk.key);
                            DUMP_CHECK_WRITE(WriteLen(tmpctx.reply.integer));
                        }
                        std::string fstr;
                        kk.element.GetDecodeString(fstr);
                        DUMP_CHECK_WRITE(WriteRawString(fstr.data(), fstr.size()));
                        DUMP_CHECK_WRITE(WriteStringObject(vv.element));
                        break;
                    }
                    case HASH_META:
                    {
                        DUMP_CHECK_WRITE(WriteLen(vv.meta.zipmap.size()));
                        DataMap::iterator it = vv.meta.zipmap.begin();
                        while (it != vv.meta.zipmap.end())
                        {
                            std::string field_name;
                            it->first.GetDecodeString(field_name);
                            DUMP_CHECK_WRITE(WriteRawString(field_name.data(), field_name.size()));
                            DUMP_CHECK_WRITE(WriteStringObject(it->second));
                            it++;
                        }
                        break;
                    }
                    case LIST_META:
                    {
                        DUMP_CHECK_WRITE(WriteLen(vv.meta.ziplist.size()));
                        DataArray::iterator it = vv.meta.ziplist.begin();
                        while (it != vv.meta.ziplist.end())
                        {
                            DUMP_CHECK_WRITE(WriteStringObject(*it));
                            it++;
                        }
                        break;
                    }
                    case ZSET_META:
                    {
                        DUMP_CHECK_WRITE(WriteLen(vv.meta.zipmap.size()));
                        DataMap::iterator it = vv.meta.zipmap.begin();
                        while (it != vv.meta.zipmap.end())
                        {
                            Data d = it->first;
                            DUMP_CHECK_WRITE(WriteStringObject(d));
                            DUMP_CHECK_WRITE(WriteDouble(it->second.NumberValue()));
                            it++;
                        }
                        break;
                    }
                    case SET_META:
                    {
                        DUMP_CHECK_WRITE(WriteLen(vv.meta.zipset.size()));
                        DataSet::iterator it = vv.meta.zipset.begin();
                        while (it != vv.meta.zipset.end())
                        {
                            DUMP_CHECK_WRITE(WriteStringObject(*it));
                            it++;
                        }
                        break;
                    }
                    default:
                    {
                        break;
                    }
                }
                iter->Next();
            }
            DELETE(iter);
        }
        if (err < 0)
        {
            ERROR_LOG("Failed to write dump file for reason code:%d", err);
            Close();
            return -1;
        }
        WriteType(REDIS_RDB_OPCODE_EOF);
        uint64 cksm = m_cksm;
        memrev64ifbe(&cksm);
        Write(&cksm, sizeof(cksm));
        Flush();
        Close();
        return 0;
    }

    RedisDumpFile::~RedisDumpFile()
    {
    }

    /*
     * Ardb dump file, used for backup data & import data
     */
    ArdbDumpFile::ArdbDumpFile()
    {
    }

    int ArdbDumpFile::WriteMagicHeader()
    {
        char magic[10];
        snprintf(magic, sizeof(magic), "ARDB%04d", ARDB_RDB_VERSION);
        return Write(magic, 8);
    }

    int ArdbDumpFile::WriteType(uint8 type)
    {
        return Write(&type, 1);
    }

    int ArdbDumpFile::SaveRawKeyValue(const Slice& key, const Slice& value)
    {
        /*
         * routine callback every 100ms
         */
        if (NULL != m_routine_cb && get_current_epoch_millis() - m_routinetime >= 100)
        {
            int cbret = m_routine_cb(m_routine_cbdata);
            if(0 != cbret)
            {
                return cbret;
            }
            m_routinetime = get_current_epoch_millis();
        }
        BufferHelper::WriteVarSlice(m_write_buffer, key);
        BufferHelper::WriteVarSlice(m_write_buffer, value);
        if (m_write_buffer.ReadableBytes() >= 1024 * 1024)
        {
            return FlushWriteBuffer();
        }
        return 0;
    }

    int ArdbDumpFile::WriteLen(uint32 len)
    {
        Buffer lenbuf(4);
        BufferHelper::WriteFixUInt32(lenbuf, len);
        return Write(lenbuf.GetRawReadBuffer(), lenbuf.ReadableBytes());
    }

    int ArdbDumpFile::ReadLen(uint32& len)
    {
        char buf[4];
        if (!Read(buf, 4, true))
        {
            return -1;
        }
        Buffer lenbuf(buf, 0, 4);
        BufferHelper::ReadFixUInt32(lenbuf, len);
        return 0;
    }

    int ArdbDumpFile::FlushWriteBuffer()
    {
        if (m_write_buffer.Readable())
        {
            std::string compressed;
            snappy::Compress(m_write_buffer.GetRawReadBuffer(), m_write_buffer.ReadableBytes(), &compressed);
            if (compressed.size() > (m_write_buffer.ReadableBytes() + 4))
            {
                RETURN_NEGATIVE_EXPR(WriteType(ARDB_RDB_TYPE_CHUNK));
                uint32 len = m_write_buffer.ReadableBytes();
                RETURN_NEGATIVE_EXPR(WriteLen(len));
                RETURN_NEGATIVE_EXPR(Write(m_write_buffer.GetRawReadBuffer(), m_write_buffer.ReadableBytes()));
            }
            else
            {
                RETURN_NEGATIVE_EXPR(WriteType(ARDB_RDB_TYPE_SNAPPY_CHUNK));
                uint32 rawlen = m_write_buffer.ReadableBytes();
                RETURN_NEGATIVE_EXPR(WriteLen(rawlen));
                RETURN_NEGATIVE_EXPR(WriteLen(compressed.size()));
                RETURN_NEGATIVE_EXPR(Write(compressed.data(), compressed.size()));
            }
            m_write_buffer.Clear();
        }
        Flush();
        return 0;
    }

    int ArdbDumpFile::DoSave()
    {
        RETURN_NEGATIVE_EXPR(WriteMagicHeader());

        KeyObject k;
        k.db = 0;
        k.type = KEY_META;
        BatchWriteGuard guard(*m_dump_ctx);
        Iterator* iter = m_db->IteratorKeyValue(k, false);
        if (NULL != iter)
        {
            while (iter->Valid())
            {
                //DelRaw(ctx, iter->Key());
                int ret = SaveRawKeyValue(iter->Key(), iter->Value());
                if(0 != ret)
                {
                    DELETE(iter);
                    Close();
                    return ret;
                }
                iter->Next();
            }
        }
        DELETE(iter);
        FlushWriteBuffer();
        WriteType(REDIS_RDB_OPCODE_EOF);
        uint64 cksm = m_cksm;
        memrev64ifbe(&cksm);
        Write(&cksm, sizeof(cksm));
        FlushWriteBuffer();
        return 0;
    }

    int ArdbDumpFile::ReadType()
    {
        unsigned char type;
        if (Read(&type, 1, true) == 0)
            return -1;
        return type;
    }

    int ArdbDumpFile::LoadBuffer(Buffer& buffer)
    {
        while (buffer.Readable())
        {
            Slice key, value;
            RETURN_NEGATIVE_EXPR(BufferHelper::ReadVarSlice(buffer, key));
            RETURN_NEGATIVE_EXPR(BufferHelper::ReadVarSlice(buffer, value));
            Context& tmpctx = *m_dump_ctx;
            if(tmpctx.identity == CONTEXT_DUMP_SYNC_LOADING)
            {
                uint32 header;
                memcpy(&header, key.data(), sizeof(header));
                DBID db = header >> 8;
                if(!g_db->m_slave.SupportDBID(db))
                {
                    continue;
                }
            }
            m_db->SetRaw(tmpctx, key, value);
        }
        return 0;
    }

    int ArdbDumpFile::DoLoad()
    {
        char buf[1024];
        int rdbver, type;
        std::string key;
        std::string verstr;
        uint32 len = 0;
        uint32 rawlen, compressedlen;
        std::string origin;
        BatchWriteGuard guard(*m_dump_ctx);
        if (!Read(buf, 8, true))
            goto eoferr;
        buf[9] = '\0';
        if (memcmp(buf, "ARDB", 4) != 0)
        {
            Close();
            WARN_LOG("Wrong signature:%s trying to load DB from file:%s", buf, m_file_path.c_str());
            return -1;
        }
        verstr.assign(buf + 4, 4);
        rdbver = atoi(verstr.c_str());
        if (rdbver < 1 || rdbver > ARDB_RDB_VERSION)
        {
            WARN_LOG("Can't handle ARDB format version %d", rdbver);
            return -1;
        }

        while (true)
        {
            /* Read type. */
            if ((type = ReadType()) == -1)
                goto eoferr;

            if (type == ARDB_RDB_TYPE_EOF)
                break;

            /* Handle SELECT DB opcode as a special case */
            if (type == ARDB_RDB_TYPE_CHUNK)
            {
                RETURN_NEGATIVE_EXPR(ReadLen(len));
                char* newbuf = NULL;
                NEW(newbuf, char[len]);
                if (!Read(newbuf, len, true))
                {
                    DELETE_A(newbuf);
                    goto eoferr;
                }
                Buffer readbuf(newbuf, 0, len);
                RETURN_NEGATIVE_EXPR(LoadBuffer(readbuf));
            }
            else if (type == ARDB_RDB_TYPE_SNAPPY_CHUNK)
            {
                RETURN_NEGATIVE_EXPR(ReadLen(rawlen));
                RETURN_NEGATIVE_EXPR(ReadLen(compressedlen));
                char* newbuf = NULL;
                NEW(newbuf, char[compressedlen]);
                if (!Read(newbuf, compressedlen, true))
                {
                    DELETE_A(newbuf);
                    goto eoferr;
                }
                origin.clear();
                origin.reserve(rawlen);
                if (!snappy::Uncompress(newbuf, compressedlen, &origin))
                {
                    DELETE_A(newbuf);
                    goto eoferr;
                }
                DELETE_A(newbuf);
                Buffer readbuf(const_cast<char*>(origin.data()), 0, origin.size());
                RETURN_NEGATIVE_EXPR(LoadBuffer(readbuf));
            }
            else
            {
                ERROR_LOG("Invalid type:%d.", type);
                goto eoferr;
            }

        }

        if (true)
        {
            /* Verify the checksum if RDB version is >= 5 */
            uint64_t cksum, expected = m_cksm;
            if (!Read(&cksum, 8, true))
            {
                goto eoferr;
            }memrev64ifbe(&cksum);
            if (cksum == 0)
            {
                WARN_LOG("RDB file was saved with checksum disabled: no check performed.");
            }
            else if (cksum != expected)
            {
                ERROR_LOG("Wrong RDB checksum.(%llu-%llu)", cksum, expected);
                //exit(1);
            }
        }

        Close();
        INFO_LOG("Ardb dump file load finished.");
        return 0;
        eoferr: Close();
        WARN_LOG("Short read or OOM loading DB. Unrecoverable error, aborting now.");
        return -1;
    }

    ArdbDumpFile::~ArdbDumpFile()
    {
    }
}
