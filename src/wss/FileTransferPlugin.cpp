// 文件分片传输插件实现 — 移植自 WebsocketServer

#include "HttpFramework/wss/FileTransferPlugin.h"
#include "HttpFramework/wss/Crc32.h"
#include "HttpFramework/wss/WssConnection.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace http {
namespace wss {

namespace {

constexpr uint8_t MSG_FILE_START  = 1;
constexpr uint8_t MSG_FILE_QUERY  = 2;
constexpr uint8_t MSG_FILE_CHUNK  = 3;
constexpr uint8_t MSG_FILE_QUERY_RESPONSE = 101;
constexpr uint8_t MSG_FILE_CHUNK_ACK       = 102;
constexpr uint8_t MSG_FILE_FINISH_ACK      = 103;
constexpr uint8_t MSG_FILE_ERROR           = 255;

// ── 网络字节序工具 ──────────────────────────────────────────────────

uint16_t readU16BE(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
uint32_t readU32BE(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint64_t readU64BE(const uint8_t* p) {
    uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | p[i]; return v;
}
void writeU32BE(std::vector<uint8_t>* out, uint32_t v) {
    out->push_back(uint8_t((v >> 24) & 0xFF)); out->push_back(uint8_t((v >> 16) & 0xFF));
    out->push_back(uint8_t((v >> 8) & 0xFF));  out->push_back(uint8_t(v & 0xFF));
}
void writeU64BE(std::vector<uint8_t>* out, uint64_t v) {
    for (int i = 7; i >= 0; --i) out->push_back(uint8_t((v >> (8*i)) & 0xFF));
}

// ── 文件名处理 ──────────────────────────────────────────────────────

std::string basenameOnly(const std::string& raw) {
    auto pos = raw.find_last_of("/\\");
    return (pos == std::string::npos) ? raw : raw.substr(pos + 1);
}
std::string sanitizeFilename(const std::string& raw) {
    std::string base = basenameOnly(raw);
    while (!base.empty() && (base.back()==' '||base.back()=='.')) base.pop_back();
    if (base.empty() || base=="." || base=="..") return "";
    std::string out; out.reserve(std::min<size_t>(base.size(), 200));
    for (unsigned char c : base) {
        if (out.size() >= 200) break;
        if (c < 32) continue;
        if (c == '/' || c == '\\' || c == ':' || c == '<' || c == '>' ||
            c == '"' || c == '|' || c == '*' || c == '?') continue;
        out.push_back(char(c));
    }
    while (!out.empty()&&(out.back()==' '||out.back()=='.')) out.pop_back();
    return out;
}
std::string makeTempPath(const std::string& dir, uint64_t id) {
    std::ostringstream oss; oss << dir << "/tmp_" << id << ".bin"; return oss.str();
}
std::string makeFinalPath(const std::string& dir, uint64_t id, const std::string& name) {
    if (name.empty()) { std::ostringstream oss; oss << dir << "/" << id << ".bin"; return oss.str(); }
    std::ostringstream oss; oss << dir << "/" << id << "_" << name; return oss.str();
}
void ensureDir(const std::string& dir) {
    struct stat st;
    if (::stat(dir.c_str(), &st)==0) { if (!S_ISDIR(st.st_mode)) throw std::runtime_error("not a dir"); return; }
    if (::mkdir(dir.c_str(),0755)<0 && errno!=EEXIST) throw std::runtime_error("mkdir failed");
}

// ── Bitmap ──────────────────────────────────────────────────────────

bool bitmapGet(const std::vector<uint8_t>& bm, uint32_t idx) {
    return ((bm[idx/8]>>(idx%8))&0x1u)!=0;
}
void bitmapSet(std::vector<uint8_t>* bm, uint32_t idx) {
    (*bm)[idx/8]|=uint8_t(1u<<(idx%8));
}

// ── 整文件 CRC32 ────────────────────────────────────────────────────

uint32_t computeFdCrc32(int fd) {
    if (::lseek(fd,0,SEEK_SET)<0) throw std::runtime_error("lseek failed");
    Crc32 crc; crc.reset();
    std::vector<uint8_t> buf(64*1024);
    while (true) {
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("read failed");
        }
        if (n == 0) break;
        crc.update(buf.data(), size_t(n));
    }
    return crc.finish();
}

}  // namespace

// ══════════════════════════════════════════════════════════════════════

FileTransferPlugin::FileTransferPlugin(const std::string& uploadDir) {
    this->uploadDir = uploadDir.empty() ? "uploads" : uploadDir;
    ensureDir(this->uploadDir);
    std::cout << "[INFO][WSS-文件]：上传目录就绪, dir=" << this->uploadDir << std::endl;
}

void FileTransferPlugin::operator()(http::WssConnection& conn, WsMessage& msg,
                                     std::function<void()> next) {
    if (!msg.isBinary()) { next(); return; }

    std::lock_guard<std::mutex> lk(mu_);
    const auto& payload = msg.payload;
    if (payload.empty()) return;

    auto err = [&]() { conn.sendBinary({MSG_FILE_ERROR}); };

    try {
        uint64_t nowSec = uint64_t(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        uint8_t msgType = payload[0];
        const uint8_t* p = payload.data()+1;
        size_t remain = payload.size()-1;

        if (msgType == MSG_FILE_START) {
            if (remain < 28) { err(); return; }
            uint64_t fid = readU64BE(p);
            uint64_t fsz = readU64BE(p+8);
            uint32_t csz = readU32BE(p+16);
            uint32_t tch = readU32BE(p+20);
            uint32_t ecr = readU32BE(p+24);
            std::string rname;
            if (remain > 28) {
                if (remain < 30) { err(); return; }
                uint16_t fl = readU16BE(p+28);
                if (fl > 512 || remain < 30u+fl) { err(); return; }
                rname.assign(reinterpret_cast<const char*>(p+30), fl);
            }
            if (tch==0 || csz==0) { err(); return; }

            auto it = states.find(fid);
            if (it != states.end()) {
                auto& ex = it->second;
                if (ex.file_size==fsz && ex.chunk_size==csz && ex.total_chunks==tch && ex.expected_crc32==ecr) {
                    std::cout << "[INFO][WSS-文件]：命中断点续传, file_id=" << fid << std::endl;
                    std::vector<uint8_t> r; r.push_back(MSG_FILE_QUERY_RESPONSE);
                    writeU64BE(&r,fid); writeU32BE(&r,tch); writeU32BE(&r,uint32_t(ex.bitmap.size()));
                    r.insert(r.end(),ex.bitmap.begin(),ex.bitmap.end());
                    conn.sendBinary(r); ex.last_update_ts=nowSec;
                    return;
                }
                if (it->second.fd>=0) ::close(it->second.fd);
                states.erase(it);
            }

            FileTransferState st;
            st.file_id=fid; st.file_size=fsz; st.chunk_size=csz; st.total_chunks=tch;
            st.expected_crc32=ecr; st.temp_path=makeTempPath(uploadDir,fid);
            st.final_display_name=sanitizeFilename(rname);
            int fd=::open(st.temp_path.c_str(),O_CREAT|O_TRUNC|O_RDWR,0644);
            if (fd<0){err();return;}
            if (::ftruncate(fd,off_t(fsz))<0){::close(fd);err();return;}
            st.fd=fd;
            size_t bmB=(size_t(tch)+7)/8; st.bitmap.assign(bmB,0);
            st.create_ts=st.last_update_ts=nowSec;
            std::cout << "[INFO][WSS-文件]：新建文件会话, file_id=" << fid
                      << " chunks=" << tch << " size=" << fsz << std::endl;
            states[fid]=st;
            std::vector<uint8_t> r; r.push_back(MSG_FILE_QUERY_RESPONSE);
            writeU64BE(&r,fid); writeU32BE(&r,tch); writeU32BE(&r,uint32_t(bmB));
            r.insert(r.end(),states[fid].bitmap.begin(),states[fid].bitmap.end());
            conn.sendBinary(r);
            return;
        }

        if (msgType == MSG_FILE_QUERY) {
            if (remain<8){err();return;}
            uint64_t fid=readU64BE(p);
            auto it=states.find(fid);
            if (it==states.end()){err();return;}
            auto& st=it->second;
            std::vector<uint8_t> r; r.push_back(MSG_FILE_QUERY_RESPONSE);
            writeU64BE(&r,fid); writeU32BE(&r,st.total_chunks); writeU32BE(&r,uint32_t(st.bitmap.size()));
            r.insert(r.end(),st.bitmap.begin(),st.bitmap.end());
            conn.sendBinary(r); st.last_update_ts=nowSec;
            return;
        }

        if (msgType == MSG_FILE_CHUNK) {
            if (remain<16){err();return;}
            uint64_t fid=readU64BE(p);
            uint32_t cid=readU32BE(p+8);
            uint32_t ccrc=readU32BE(p+12);
            auto it=states.find(fid);
            if (it==states.end()){err();return;}
            auto& st=it->second;
            if (cid>=st.total_chunks){err();return;}
            const uint8_t* data=p+16; size_t dlen=remain-16;
            Crc32 crc;
            if (crc.checksum(data,dlen)!=ccrc) {
                std::cerr << "[WARN][WSS-文件]：分片CRC校验失败, file_id=" << fid
                          << " chunk=" << cid << std::endl;
                std::vector<uint8_t> a; a.push_back(MSG_FILE_CHUNK_ACK);
                writeU64BE(&a,fid); writeU32BE(&a,cid); a.push_back(0);
                conn.sendBinary(a); return;
            }
            off_t off=off_t(cid)*off_t(st.chunk_size);
            ssize_t pw = ::pwrite(st.fd, data, dlen, off);
            (void)pw;  // 写入结果由 CRC32 校验保证
            if (!bitmapGet(st.bitmap,cid)) {
                bitmapSet(&st.bitmap,cid); st.received_count++; st.last_update_ts=nowSec;
            }
            std::vector<uint8_t> a; a.push_back(MSG_FILE_CHUNK_ACK);
            writeU64BE(&a,fid); writeU32BE(&a,cid); a.push_back(1);
            conn.sendBinary(a);

            if (st.received_count==st.total_chunks) {
                uint32_t fcrc=computeFdCrc32(st.fd);
                ::close(st.fd); st.fd=-1;
                bool ok=(fcrc==st.expected_crc32);
                if (ok) {
                    std::string fp=makeFinalPath(uploadDir,st.file_id,st.final_display_name);
                    ::rename(st.temp_path.c_str(),fp.c_str());
                    std::vector<uint8_t> f; f.push_back(MSG_FILE_FINISH_ACK);
                    writeU64BE(&f,fid); f.push_back(1);
                    conn.sendBinary(f);
                    std::cout << "[INFO][WSS-文件]：传输完成, file_id=" << fid << " path=" << fp << std::endl;
                } else {
                    ::unlink(st.temp_path.c_str());
                    std::vector<uint8_t> f; f.push_back(MSG_FILE_FINISH_ACK);
                    writeU64BE(&f,fid); f.push_back(0);
                    conn.sendBinary(f);
                    std::cerr << "[ERROR][WSS-文件]：整文件CRC校验失败, file_id=" << fid << std::endl;
                }
                states.erase(fid);
            }
            return;
        }
    } catch (const std::exception& ex) {
        std::cerr << "[ERROR][WSS-文件]：异常: " << ex.what() << std::endl;
    }
}

void FileTransferPlugin::cleanup(uint64_t nowSec, uint64_t ttlSec, std::size_t maxSessions) {
    std::lock_guard<std::mutex> lk(mu_);

    std::vector<uint64_t> expired;
    for (auto& kv : states)
        if (nowSec > kv.second.last_update_ts && (nowSec-kv.second.last_update_ts) > ttlSec)
            expired.push_back(kv.first);
    for (auto id : expired) {
        auto it = states.find(id); if (it==states.end()) continue;
        if (it->second.fd>=0) ::close(it->second.fd);
        ::unlink(it->second.temp_path.c_str()); states.erase(it);
        std::cout << "[INFO][WSS-文件]：清理过期会话, file_id=" << id << std::endl;
    }

    if (states.size() <= maxSessions) return;
    std::vector<std::pair<uint64_t,uint64_t>> sorted;
    for (auto& kv : states) sorted.push_back({kv.first,kv.second.last_update_ts});
    std::sort(sorted.begin(),sorted.end(),[](auto& a,auto& b){return a.second<b.second;});
    for (size_t i=0, n=states.size()-maxSessions; i<n; ++i) {
        auto it=states.find(sorted[i].first); if (it==states.end()) continue;
        if (it->second.fd>=0) ::close(it->second.fd);
        ::unlink(it->second.temp_path.c_str()); states.erase(it);
    }
}

}  // namespace wss
}  // namespace http
