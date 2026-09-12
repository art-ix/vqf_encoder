#include "stdafx.h"

#include "twinvq/twinvq_encoder.hpp"
#include "twinvq/vqf_file.hpp"

#include <memory>
#include <string>
#include <vector>

namespace {

const GUID guid_branch = {0x5e91c2aa, 0x17b4, 0x4c8e, {0x9d, 0x3a, 0x6b, 0x11, 0x44, 0x8f, 0x20, 0x77}};
const GUID guid_bitrate = {0x5e91c2ab, 0x17b4, 0x4c8e, {0x9d, 0x3a, 0x6b, 0x11, 0x44, 0x8f, 0x20, 0x77}};

advconfig_branch_factory g_branch("TwinVQ encoder", guid_branch, advconfig_entry::guid_root, 0);
advconfig_integer_factory g_bitrate("Bitrate (kbps, total)", guid_bitrate, guid_branch, 0, 96, 16, 192);

int pick_bitrate(int sample_rate, int channels) {
    int want = static_cast<int>(g_bitrate.get());
    int out_rate = 0, out_br = 0;
    std::string err;
    if (!twinvq::pick_encoder_mode(sample_rate, channels, want, out_rate, out_br, err))
        return want;
    return out_br;
}

class vqf_instance : public fb2k::audioEncoderInstance {
public:
    vqf_instance(file::ptr out, const fb2k::audioEncoderSetup_t& setup, abort_callback&)
        : m_out(out) {
        const int ch = static_cast<int>(setup.spec.chanCount);
        const int rate = static_cast<int>(setup.spec.sampleRate);
        twinvq::Encoder::Config cfg;
        cfg.sample_rate = rate;
        cfg.channels = ch;
        cfg.bitrate_kbps = pick_bitrate(rate, ch);
        cfg.compensate_delay = true;
        m_enc = std::make_unique<twinvq::Encoder>(cfg);
        m_spec = setup.spec;
    }

    void addChunk(const audio_chunk& chunk, abort_callback&) override {
        const t_size frames = chunk.get_sample_count();
        const unsigned ch = chunk.get_channel_count();
        if (ch != static_cast<unsigned>(m_enc->channels()))
            throw std::runtime_error("TwinVQ encoder channel count changed");
        const audio_sample* src = chunk.get_data();
        m_pcm.resize(frames * ch);
        for (t_size i = 0; i < frames * ch; i++)
            m_pcm[i] = static_cast<float>(src[i]);
        m_enc->feed(m_pcm.data(), static_cast<int>(frames));
    }

    void finalize(abort_callback& abort) override {
        m_enc->flush();
        const auto file = m_enc->build_file();
        m_out->write(file.data(), file.size(), abort);
    }

private:
    file::ptr m_out;
    audio_chunk::spec_t m_spec{};
    std::unique_ptr<twinvq::Encoder> m_enc;
    std::vector<float> m_pcm;
};

class vqf_encoder : public fb2k::audioEncoder {
public:
    const char* formatExtension() override { return "vqf"; }

    fb2k::audioEncoderInstance::ptr open(file::ptr outFile, fb2k::audioEncoderSetup_t const& spec,
                                         abort_callback& aborter) override {
        return new service_impl_t<vqf_instance>(outFile, spec, aborter);
    }
};

service_factory_single_t<vqf_encoder> g_vqf_encoder;

} // namespace
