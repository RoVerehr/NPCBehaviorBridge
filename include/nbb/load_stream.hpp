#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nbb::load_stream {
// Flat SE/AE TESFile/NiFile layouts, checked against pinned CommonLib and all
// supported runtime descriptors; Steam1170 additionally verified in live code.
// Only copies bytes already owned by the engine;
// never seeks, reads a file, decompresses, or calls an engine function.
constexpr std::size_t maxPayload = 32 * 1024 * 1024;
struct Payload {
    std::uint32_t id{}, flags{}, recordSize{}, fileOffset{};
    std::vector<std::uint8_t> bytes;
    std::string source, error;
    bool complete{};
};
template<class T, std::size_t N> T word(const std::array<std::uint8_t,N>& b,std::size_t offset) {
    T value{};std::memcpy(&value,b.data()+offset,sizeof(value));return value;
}
template<class Read> Payload capture(std::uintptr_t address,Read&& read) {
    Payload out;
    std::array<std::uint8_t,0x4C8> file{};
    if(!read(address,file.data(),file.size())) {out.error="Unreadable TESFile";return out;}
    out.id=word<std::uint32_t>(file,0x290);out.flags=word<std::uint32_t>(file,0x28C);
    out.recordSize=word<std::uint32_t>(file,0x288);out.fileOffset=word<std::uint32_t>(file,0x2A8);
    if(word<std::uint32_t>(file,0x284)!=0x5F43504E || !out.id || (out.id>>24)==0xFF) {
        out.error="Not a static NPC record";return out;
    }
    if(file[0x2E9]) {out.error="Unsupported byte order";return out;}
    auto copy=[&](std::uintptr_t ptr,std::size_t count,const char* source) {
        if(count>maxPayload || (count && (!ptr || ptr>UINTPTR_MAX-count)))return false;
        out.bytes.resize(count);
        if(count && !read(ptr,out.bytes.data(),count)) {out.bytes.clear();return false;}
        out.source=source;out.complete=true;return true;
    };
    // Reuse only resident raw file bytes. This also allows verifying the
    // uncompressed-length prefix of an empty compressed deletion record.
    auto resident=[&](std::uint64_t begin,void* target,std::size_t count) {
        const auto mapped=word<std::uintptr_t>(file,0x18);
        const auto mappedSize=word<std::uint32_t>(file,0x20);
        if(file[0x29] && mapped && begin<=mappedSize && count<=mappedSize-begin &&
            mapped<=UINTPTR_MAX-begin && read(mapped+begin,target,count)) {out.source="engine_file_memory";return true;}
        std::array<std::uint8_t,0x180> bs{};
        if(read(word<std::uintptr_t>(file,0x38),bs.data(),bs.size()) && bs[0x44]) {
            const auto total=word<std::uint32_t>(bs,0x2C),available=word<std::uint32_t>(bs,0x24);
            const auto allocation=word<std::uint32_t>(bs,0x20),position=word<std::uint32_t>(bs,0x28);
            const auto buffer=word<std::uintptr_t>(bs,0x30);
            if(available<=allocation && available<=total && position<=available && begin>=total-available &&
                begin<=total && count<=total-begin && buffer && buffer<=UINTPTR_MAX-(begin-(total-available)) &&
                read(buffer+begin-(total-available),target,count)) {out.source="engine_read_ahead";return true;}
        }
        return false;
    };
    if(out.flags&0x40000) {
        const auto ptr=word<std::uintptr_t>(file,0x4A0);
        const auto count=word<std::uint32_t>(file,0x4A8);
        if(ptr && count && copy(ptr,count,"engine_decompressed"))return out;
        std::uint32_t declared=UINT32_MAX;
        // A deletion/ignored header does not prove that its payload is empty.
        // Require the actual resident file prefix to declare zero output bytes.
        if((out.flags&(0x20|0x1000)) && !count && out.recordSize>=4 && out.recordSize<=maxPayload &&
            resident(std::uint64_t(out.fileOffset)+24,&declared,4) && declared==0) {
            out.bytes.clear();out.source="empty_inactive_record";out.complete=true;return out;
        }
        out.error="Decompressed NPC buffer unavailable or exceeds limit";return out;
    }
    if(out.recordSize>maxPayload) {out.error="NPC payload exceeds limit";return out;}
    if(!out.recordSize && (out.flags&(0x20|0x1000))) {
        out.source="empty_inactive_record";out.complete=true;return out;
    }
    out.bytes.resize(out.recordSize);
    if(resident(std::uint64_t(out.fileOffset)+24,out.bytes.data(),out.bytes.size())) {out.complete=true;return out;}
    out.bytes.clear();
    out.error="Plain NPC is not fully resident in an engine buffer";return out;
}
}
