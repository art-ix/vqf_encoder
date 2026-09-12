#include "twinvq/vqf_file.hpp"
#include "twinvq_tables.hpp"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace twinvq {
namespace {

uint32_t rd_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

int32_t rd_bei32(const uint8_t* p) {
    return static_cast<int32_t>(rd_be32(p));
}

bool read_exact(const ReadFn& read, void* dst, size_t n) {
    return read(dst, n) == n;
}

void wr_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

bool id_eq(const char id[4], const char* s) {
    return std::memcmp(id, s, 4) == 0;
}

struct TagField {
    const char* id;
    std::string VqfInfo::*info;
    std::string VqfTags::*tags;
};

constexpr TagField kTagFields[] = {
    {"NAME", &VqfInfo::title, &VqfTags::title},
    {"AUTH", &VqfInfo::artist, &VqfTags::artist},
    {"COMT", &VqfInfo::comment, &VqfTags::comment},
    {"(c) ", &VqfInfo::copyright, &VqfTags::copyright},
    {"ALBM", &VqfInfo::album, &VqfTags::album},
    {"GENR", &VqfInfo::genre, &VqfTags::genre},
    {"TRCK", &VqfInfo::track, &VqfTags::track},
    {"YEAR", &VqfInfo::year, &VqfTags::year},
    {"MUSC", &VqfInfo::composer, &VqfTags::composer},
    {"LABL", &VqfInfo::publisher, &VqfTags::publisher},
};

const TagField* find_tag_field(const char id[4]) {
    const char* canonical_id = id_eq(id, "TRAC") ? "TRCK" : id;
    for (const auto& field : kTagFields)
        if (id_eq(canonical_id, field.id))
            return &field;
    return nullptr;
}

std::string payload_text(const std::vector<uint8_t>& p) {
    return std::string(reinterpret_cast<const char*>(p.data()), p.size());
}

void apply_chunk_meta(VqfInfo& info, const VqfChunk& c) {
    const auto* field = find_tag_field(c.id);
    if (!field)
        return;
    auto& text = info.*field->info;
    if (field->info == &VqfInfo::track && c.payload.size() == 2) {
        const int t = (c.payload[0] << 8) | c.payload[1];
        text = std::to_string(t);
    } else if (field->info == &VqfInfo::year && c.payload.size() >= 2) {
        const int y = (c.payload[0] << 8) | c.payload[1];
        if (y > 0 && y < 3000 && c.payload.size() <= 4)
            text = std::to_string(y);
        else
            text = payload_text(c.payload);
    } else {
        text = payload_text(c.payload);
    }
}

void refresh_meta(VqfInfo& info) {
    for (const auto& field : kTagFields)
        (info.*field.info).clear();
    for (const auto& c : info.chunks)
        apply_chunk_meta(info, c);
}

size_t header_byte_size(const VqfInfo& info) {
    size_t size = 20; // TWIN + version + size + DATA marker.
    for (const auto& c : info.chunks)
        size += 8 + c.payload.size();
    return size;
}

VqfChunk make_text_chunk(const char* id, const std::string& text) {
    VqfChunk c;
    std::memcpy(c.id, id, 4);
    c.payload.assign(text.begin(), text.end());
    return c;
}

VqfChunk make_dsiz(uint32_t data_size) {
    VqfChunk c;
    std::memcpy(c.id, "DSIZ", 4);
    c.payload.resize(4);
    wr_be32(c.payload.data(), data_size);
    return c;
}

const ModeTab kMode08_08{
    {
        {8, tables::bark_tab_s08_64, 10, tables::fcb08s, 1, 5, tables::cb0808s0, tables::cb0808s1, 18},
        {2, tables::bark_tab_m08_256, 20, tables::fcb08m, 2, 5, tables::cb0808m0, tables::cb0808m1, 16},
        {1, tables::bark_tab_l08_512, 30, tables::fcb08l, 3, 6, tables::cb0808l0, tables::cb0808l1, 17},
    },
    512, 12, tables::ff_metasound_lsp8, 1, 5, 3, 3, tables::shape08, 8, 28, 20, 6, 40};

const ModeTab kMode11_08{
    {
        {8, tables::bark_tab_s11_64, 10, tables::fcb11s, 1, 5, tables::cb1108s0, tables::cb1108s1, 29},
        {2, tables::bark_tab_m11_256, 20, tables::fcb11m, 2, 5, tables::cb1108m0, tables::cb1108m1, 24},
        {1, tables::bark_tab_l11_512, 30, tables::fcb11l, 3, 6, tables::cb1108l0, tables::cb1108l1, 27},
    },
    512, 16, tables::ff_metasound_lsp11, 1, 6, 4, 3, tables::shape11, 9, 36, 30, 7, 90};

const ModeTab kMode11_10{
    {
        {8, tables::bark_tab_s11_64, 10, tables::fcb11s, 1, 5, tables::cb1110s0, tables::cb1110s1, 21},
        {2, tables::bark_tab_m11_256, 20, tables::fcb11m, 2, 5, tables::cb1110m0, tables::cb1110m1, 18},
        {1, tables::bark_tab_l11_512, 30, tables::fcb11l, 3, 6, tables::cb1110l0, tables::cb1110l1, 20},
    },
    512, 16, tables::ff_metasound_lsp11, 1, 6, 4, 3, tables::shape11, 9, 36, 30, 7, 90};

const ModeTab kMode16_16{
    {
        {8, tables::bark_tab_s16_128, 10, tables::fcb16s, 1, 5, tables::cb1616s0, tables::cb1616s1, 16},
        {2, tables::bark_tab_m16_512, 20, tables::fcb16m, 2, 5, tables::cb1616m0, tables::cb1616m1, 15},
        {1, tables::bark_tab_l16_1024, 30, tables::fcb16l, 3, 6, tables::cb1616l0, tables::cb1616l1, 16},
    },
    1024, 16, tables::ff_metasound_lsp16, 1, 6, 4, 3, tables::shape16, 9, 56, 60, 7, 180};

const ModeTab kMode22_20{
    {
        {8, tables::bark_tab_s22_128, 10, tables::fcb22s_1, 1, 6, tables::cb2220s0, tables::cb2220s1, 18},
        {2, tables::bark_tab_m22_512, 20, tables::fcb22m_1, 2, 6, tables::cb2220m0, tables::cb2220m1, 17},
        {1, tables::bark_tab_l22_1024, 32, tables::fcb22l_1, 4, 6, tables::cb2220l0, tables::cb2220l1, 18},
    },
    1024, 16, tables::ff_metasound_lsp22, 1, 6, 4, 3, tables::shape22_1, 9, 56, 36, 7, 144};

const ModeTab kMode22_24{
    {
        {8, tables::bark_tab_s22_128, 10, tables::fcb22s_1, 1, 6, tables::cb2224s0, tables::cb2224s1, 15},
        {2, tables::bark_tab_m22_512, 20, tables::fcb22m_1, 2, 6, tables::cb2224m0, tables::cb2224m1, 14},
        {1, tables::bark_tab_l22_1024, 32, tables::fcb22l_1, 4, 6, tables::cb2224l0, tables::cb2224l1, 15},
    },
    1024, 16, tables::ff_metasound_lsp22, 1, 6, 4, 3, tables::shape22_1, 9, 56, 36, 7, 144};

const ModeTab kMode22_32{
    {
        {4, tables::bark_tab_s22_128, 10, tables::fcb22s_2, 1, 6, tables::cb2232s0, tables::cb2232s1, 11},
        {2, tables::bark_tab_m22_256, 20, tables::fcb22m_2, 2, 6, tables::cb2232m0, tables::cb2232m1, 11},
        {1, tables::bark_tab_l22_512, 32, tables::fcb22l_2, 4, 6, tables::cb2232l0, tables::cb2232l1, 12},
    },
    512, 16, tables::lsp22_2, 1, 6, 4, 4, tables::shape22_2, 9, 56, 36, 7, 72};

const ModeTab kMode44_40{
    {
        {16, tables::bark_tab_s44_128, 10, tables::fcb44s, 1, 6, tables::cb4440s0, tables::cb4440s1, 18},
        {4, tables::bark_tab_m44_512, 20, tables::fcb44m, 2, 6, tables::cb4440m0, tables::cb4440m1, 17},
        {1, tables::bark_tab_l44_2048, 40, tables::fcb44l, 4, 6, tables::cb4440l0, tables::cb4440l1, 17},
    },
    2048, 20, tables::ff_metasound_lsp44, 1, 6, 4, 4, tables::shape44, 9, 84, 54, 7, 432};

const ModeTab kMode44_48{
    {
        {16, tables::bark_tab_s44_128, 10, tables::fcb44s, 1, 6, tables::cb4448s0, tables::cb4448s1, 15},
        {4, tables::bark_tab_m44_512, 20, tables::fcb44m, 2, 6, tables::cb4448m0, tables::cb4448m1, 14},
        {1, tables::bark_tab_l44_2048, 40, tables::fcb44l, 4, 6, tables::cb4448l0, tables::cb4448l1, 14},
    },
    2048, 20, tables::ff_metasound_lsp44, 1, 6, 4, 4, tables::shape44, 9, 84, 54, 7, 432};

} // namespace

const ModeTab* select_mode(int sample_rate, int bitrate_kbps, int channels) {
    if (channels < 1)
        return nullptr;
    const int isampf = (sample_rate == 44100) ? 44 : (sample_rate == 22050) ? 22 : (sample_rate == 11025) ? 11 : sample_rate / 1000;
    const int ibps = bitrate_kbps / channels;
    const int key = (isampf << 8) + ibps;
    switch (key) {
    case (8 << 8) + 8:
        return &kMode08_08;
    case (11 << 8) + 8:
        return &kMode11_08;
    case (11 << 8) + 10:
        return &kMode11_10;
    case (16 << 8) + 16:
        return &kMode16_16;
    case (22 << 8) + 20:
        return &kMode22_20;
    case (22 << 8) + 24:
        return &kMode22_24;
    case (22 << 8) + 32:
        return &kMode22_32;
    case (44 << 8) + 40:
        return &kMode44_40;
    case (44 << 8) + 48:
        return &kMode44_48;
    default:
        return nullptr;
    }
}

const LegalMode* legal_modes(int& count) {
    static const LegalMode kModes[] = {
        {8000, 8, 512},   {11025, 8, 512},  {11025, 10, 512}, {16000, 16, 1024},
        {22050, 20, 1024}, {22050, 24, 1024}, {22050, 32, 512}, {44100, 40, 2048},
        {44100, 48, 2048},
    };
    count = static_cast<int>(sizeof(kModes) / sizeof(kModes[0]));
    return kModes;
}


bool parse_vqf_header(const ReadFn& read, VqfInfo& info, std::string& error) {
    info = VqfInfo{};
    uint8_t magic[12];
    if (!read_exact(read, magic, 12)) {
        error = "truncated header";
        return false;
    }
    if (std::memcmp(magic, "TWIN", 4) != 0) {
        error = "not a TwinVQ/VQF file";
        return false;
    }
    info.version.assign(reinterpret_cast<char*>(magic + 4), 8);
    if (info.version != "97012000" && info.version != "00052200") {
        error = "unsupported VQF version " + info.version;
        return false;
    }

    uint8_t szb[4];
    if (!read_exact(read, szb, 4)) {
        error = "truncated header size";
        return false;
    }
    int header_size = static_cast<int>(rd_be32(szb));
    if (header_size < 0) {
        error = "invalid header size";
        return false;
    }

    bool have_comm = false;
    int64_t consumed = 16; // TWIN+ver+size
    while (header_size >= 0) {
        uint8_t id[4];
        if (!read_exact(read, id, 4)) {
            error = "truncated chunk id";
            return false;
        }
        consumed += 4;
        if (std::memcmp(id, "DATA", 4) == 0)
            break;
        uint8_t lenb[4];
        if (!read_exact(read, lenb, 4)) {
            error = "truncated chunk size";
            return false;
        }
        const uint32_t len = rd_be32(lenb);
        consumed += 4;
        header_size -= 8;
        if (static_cast<int>(len) > header_size && header_size >= 0) {
            // still try to read `len` as per format; clamp if insane
        }
        std::vector<uint8_t> payload(len);
        if (len && !read_exact(read, payload.data(), len)) {
            error = "truncated chunk payload";
            return false;
        }
        consumed += len;
        header_size -= static_cast<int>(len);

        VqfChunk chunk;
        std::memcpy(chunk.id, id, 4);
        chunk.payload = std::move(payload);
        info.chunks.push_back(chunk);

        if (id_eq(chunk.id, "COMM")) {
            if (chunk.payload.size() < 12) {
                error = "COMM chunk too small";
                return false;
            }
            info.channels = rd_bei32(chunk.payload.data()) + 1;
            info.bitrate_kbps = rd_bei32(chunk.payload.data() + 4);
            const int rate_flag = rd_bei32(chunk.payload.data() + 8);
            if (rate_flag == 44)
                info.sample_rate = 44100;
            else if (rate_flag == 22)
                info.sample_rate = 22050;
            else if (rate_flag == 11)
                info.sample_rate = 11025;
            else if (rate_flag >= 8 && rate_flag <= 44)
                info.sample_rate = rate_flag * 1000;
            else {
                error = "invalid sample rate flag";
                return false;
            }
            have_comm = true;
        } else if (id_eq(chunk.id, "DSIZ")) {
            if (chunk.payload.size() >= 4)
                info.data_size = rd_be32(chunk.payload.data());
        }
    }

    if (!have_comm) {
        error = "missing COMM chunk";
        return false;
    }
    if (info.channels < 1 || info.channels > 2) {
        error = "unsupported channel count";
        return false;
    }
    const ModeTab* mode = select_mode(info.sample_rate, info.bitrate_kbps, info.channels);
    if (!mode) {
        error = "unsupported TwinVQ mode";
        return false;
    }
    info.frame_samples = mode->size;
    info.frame_bits = static_cast<int>(static_cast<int64_t>(info.bitrate_kbps) * 1000 * mode->size / info.sample_rate);
    info.data_offset = static_cast<uint64_t>(consumed);
    refresh_meta(info);
    return true;
}

std::vector<uint8_t> serialize_vqf_header(const VqfInfo& info) {
    const size_t header_size = header_byte_size(info);
    std::vector<uint8_t> out;
    out.reserve(header_size);
    out.insert(out.end(), {'T', 'W', 'I', 'N'});
    std::string ver = info.version.empty() ? std::string("97012000") : info.version;
    if (ver.size() < 8)
        ver.append(8 - ver.size(), '0');
    out.insert(out.end(), ver.begin(), ver.begin() + 8);
    uint8_t sz[4];
    wr_be32(sz, static_cast<uint32_t>(header_size - 20));
    out.insert(out.end(), sz, sz + 4);
    for (const auto& c : info.chunks) {
        out.insert(out.end(), c.id, c.id + 4);
        wr_be32(sz, static_cast<uint32_t>(c.payload.size()));
        out.insert(out.end(), sz, sz + 4);
        out.insert(out.end(), c.payload.begin(), c.payload.end());
    }
    out.insert(out.end(), {'D', 'A', 'T', 'A'});
    return out;
}

void apply_tags(VqfInfo& info, const VqfTags& tags, bool strip_all) {
    VqfChunk comm{};
    bool have_comm = false;
    std::vector<VqfChunk> extra;
    for (const auto& c : info.chunks) {
        if (id_eq(c.id, "COMM")) {
            comm = c;
            have_comm = true;
        } else if (id_eq(c.id, "DSIZ") || find_tag_field(c.id)) {
            continue;
        } else {
            extra.push_back(c);
        }
    }

    info.chunks.clear();
    if (have_comm)
        info.chunks.push_back(std::move(comm));

    if (!strip_all) {
        for (const auto& field : kTagFields) {
            const auto& text = tags.*field.tags;
            if (!text.empty())
                info.chunks.push_back(make_text_chunk(field.id, text));
        }
    }

    info.chunks.insert(info.chunks.end(), extra.begin(), extra.end());
    info.chunks.push_back(make_dsiz(static_cast<uint32_t>(info.data_size)));
    refresh_meta(info);
    info.data_offset = header_byte_size(info);
}

bool parse_vqf_header_mem(const uint8_t* data, size_t size, VqfInfo& info, std::string& error) {
    size_t pos = 0;
    auto read = [&](void* dst, size_t n) -> size_t {
        if (pos + n > size)
            n = size - pos;
        std::memcpy(dst, data + pos, n);
        pos += n;
        return n;
    };
    return parse_vqf_header(read, info, error);
}

namespace {

void wr_be32_pub(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

} // namespace

VqfChunk make_comm_chunk(int channels, int bitrate_kbps, int sample_rate) {
    VqfChunk c;
    std::memcpy(c.id, "COMM", 4);
    c.payload.resize(12);
    wr_be32_pub(c.payload.data(), static_cast<uint32_t>(channels - 1));
    wr_be32_pub(c.payload.data() + 4, static_cast<uint32_t>(bitrate_kbps));
    int rate_flag = sample_rate / 1000;
    if (sample_rate == 44100)
        rate_flag = 44;
    else if (sample_rate == 22050)
        rate_flag = 22;
    else if (sample_rate == 11025)
        rate_flag = 11;
    wr_be32_pub(c.payload.data() + 8, static_cast<uint32_t>(rate_flag));
    return c;
}

VqfChunk make_dsiz_chunk(uint32_t data_size) {
    VqfChunk c;
    std::memcpy(c.id, "DSIZ", 4);
    c.payload.resize(4);
    wr_be32_pub(c.payload.data(), data_size);
    return c;
}

VqfInfo make_vqf_info(int channels, int sample_rate, int bitrate_kbps, const VqfTags& tags,
                      uint64_t data_size, const std::string& version) {
    VqfInfo info;
    info.channels = channels;
    info.sample_rate = sample_rate;
    info.bitrate_kbps = bitrate_kbps;
    info.version = version.empty() ? std::string("97012000") : version;
    info.data_size = data_size;
    const ModeTab* mode = select_mode(sample_rate, bitrate_kbps, channels);
    if (mode) {
        info.frame_samples = mode->size;
        info.frame_bits = static_cast<int>(static_cast<int64_t>(bitrate_kbps) * 1000 * mode->size / sample_rate);
    }
    info.chunks.push_back(make_comm_chunk(channels, bitrate_kbps, sample_rate));
    apply_tags(info, tags, false);
    return info;
}

std::vector<uint8_t> build_vqf_file(const VqfInfo& info, const uint8_t* data, size_t data_size) {
    VqfInfo copy = info;
    copy.data_size = data_size;
    bool have_dsiz = false;
    for (auto& c : copy.chunks) {
        if (std::memcmp(c.id, "DSIZ", 4) == 0) {
            c = make_dsiz_chunk(static_cast<uint32_t>(data_size));
            have_dsiz = true;
        }
    }
    if (!have_dsiz)
        copy.chunks.push_back(make_dsiz_chunk(static_cast<uint32_t>(data_size)));
    std::vector<uint8_t> out = serialize_vqf_header(copy);
    if (data && data_size)
        out.insert(out.end(), data, data + data_size);
    return out;
}

} // namespace twinvq
