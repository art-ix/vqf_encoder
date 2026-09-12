#include "stdafx.h"
#include "resource.h"

#include "twinvq/twinvq_encoder.hpp"
#include "twinvq/vqf_file.hpp"

#include <memory>
#include <string>
#include <vector>

namespace {

const GUID guid_branch = {0x5e91c2aa, 0x17b4, 0x4c8e, {0x9d, 0x3a, 0x6b, 0x11, 0x44, 0x8f, 0x20, 0x77}};
const GUID guid_bitrate = {0x5e91c2ab, 0x17b4, 0x4c8e, {0x9d, 0x3a, 0x6b, 0x11, 0x44, 0x8f, 0x20, 0x77}};
const GUID guid_convert = {0x5e91c2ac, 0x17b4, 0x4c8e, {0x9d, 0x3a, 0x6b, 0x11, 0x44, 0x8f, 0x20, 0x77}};
const GUID guid_dest_same = {0x5e91c2ad, 0x17b4, 0x4c8e, {0x9d, 0x3a, 0x6b, 0x11, 0x44, 0x8f, 0x20, 0x77}};

advconfig_branch_factory g_branch("TwinVQ encoder", guid_branch, advconfig_entry::guid_root, 0);
advconfig_integer_factory g_bitrate("Bitrate (kbps, total)", guid_bitrate, guid_branch, 0, 96, 16, 96);
advconfig_checkbox_factory g_dest_same("Write next to source files", guid_dest_same, guid_branch, 1, true);

const int k_bitrates[] = {16, 32, 40, 48, 64, 80, 96};
const int k_bitrate_count = 7;

int pick_bitrate(int sample_rate, int channels) {
    int want = static_cast<int>(g_bitrate.get());
    int out_rate = 0, out_br = 0;
    std::string err;
    if (!twinvq::pick_encoder_mode(sample_rate, channels, want, out_rate, out_br, err))
        return want;
    return out_br;
}

std::string meta1(const file_info& info, const char* key) {
    const char* v = info.meta_get(key, 0);
    return v ? v : "";
}

twinvq::VqfTags tags_from_info(const file_info& info) {
    twinvq::VqfTags tags;
    tags.title = meta1(info, "title");
    tags.artist = meta1(info, "artist");
    tags.album = meta1(info, "album");
    tags.genre = meta1(info, "genre");
    tags.comment = meta1(info, "comment");
    tags.composer = meta1(info, "composer");
    tags.publisher = meta1(info, "publisher");
    tags.copyright = meta1(info, "copyright");
    tags.track = meta1(info, "tracknumber");
    tags.year = meta1(info, "date");
    if (tags.year.size() > 4)
        tags.year.resize(4);
    return tags;
}

void to_encoder_channels(const audio_chunk& in, int out_ch, audio_chunk_impl& out) {
    const t_size frames = in.get_sample_count();
    const unsigned in_ch = in.get_channel_count();
    const unsigned cfg = out_ch == 1 ? audio_chunk::channel_config_mono : audio_chunk::channel_config_stereo;
    if (in_ch == static_cast<unsigned>(out_ch)) {
        out.set_data(in.get_data(), frames, out_ch, in.get_sample_rate(), cfg);
        return;
    }
    pfc::array_t<audio_sample> buf;
    buf.set_size(frames * static_cast<t_size>(out_ch));
    const audio_sample* src = in.get_data();
    for (t_size i = 0; i < frames; ++i) {
        for (int c = 0; c < out_ch; ++c) {
            const unsigned ic = static_cast<unsigned>(c) < in_ch ? static_cast<unsigned>(c) : in_ch - 1;
            buf[i * static_cast<t_size>(out_ch) + c] = src[i * in_ch + ic];
        }
    }
    out.set_data(buf.get_ptr(), frames, out_ch, in.get_sample_rate(), cfg);
}

class encoder_pipe {
public:
    encoder_pipe(int src_rate, int src_ch, const twinvq::VqfTags& tags) {
        const int ch = src_ch >= 2 ? 2 : 1;
        twinvq::Encoder::Config cfg;
        cfg.sample_rate = src_rate;
        cfg.channels = ch;
        cfg.bitrate_kbps = pick_bitrate(src_rate, ch);
        cfg.compensate_delay = true;
        cfg.tags = tags;
        m_enc = std::make_unique<twinvq::Encoder>(cfg);
        m_ch = m_enc->channels();
        m_rate = m_enc->sample_rate();
        if (src_rate != m_rate) {
            if (!resampler_entry::g_create(m_rs, static_cast<unsigned>(src_rate), static_cast<unsigned>(m_rate), 1.0f))
                throw std::runtime_error("no resampler for TwinVQ sample rate");
        }
    }

    int channels() const { return m_ch; }
    int sample_rate() const { return m_rate; }

    void add_chunk(const audio_chunk& chunk, abort_callback& abort) {
        abort.check();
        if (chunk.get_sample_count() == 0)
            return;
        audio_chunk_impl ch;
        to_encoder_channels(chunk, m_ch, ch);
        if (m_rs.is_empty()) {
            feed_pcm(ch);
            return;
        }
        dsp_chunk_list_impl list;
        list.add_chunk(&ch);
        m_rs->run(&list, dsp_track_t(), 0);
        drain(list);
    }

    void finalize(file::ptr out, abort_callback& abort) {
        if (m_rs.is_valid()) {
            dsp_chunk_list_impl list;
            m_rs->run(&list, dsp_track_t(), dsp::FLUSH);
            drain(list);
        }
        m_enc->flush();
        const auto bytes = m_enc->build_file();
        out->write(bytes.data(), bytes.size(), abort);
        out->commit(abort);
    }

private:
    void drain(dsp_chunk_list_impl& list) {
        const t_size n = list.get_count();
        for (t_size i = 0; i < n; ++i)
            feed_pcm(*list.get_item(i));
        list.remove_all();
    }

    void feed_pcm(const audio_chunk& chunk) {
        const t_size frames = chunk.get_sample_count();
        const unsigned ch = chunk.get_channel_count();
        if (ch != static_cast<unsigned>(m_ch))
            throw std::runtime_error("TwinVQ encoder channel count changed");
        const audio_sample* src = chunk.get_data();
        m_pcm.resize(frames * ch);
        for (t_size i = 0; i < frames * ch; ++i)
            m_pcm[i] = static_cast<float>(src[i]);
        m_enc->feed(m_pcm.data(), static_cast<int>(frames));
    }

    std::unique_ptr<twinvq::Encoder> m_enc;
    service_ptr_t<dsp> m_rs;
    int m_ch = 0;
    int m_rate = 0;
    std::vector<float> m_pcm;
};

class vqf_instance : public fb2k::audioEncoderInstance {
public:
    vqf_instance(file::ptr out, const fb2k::audioEncoderSetup_t& setup, abort_callback&)
        : m_out(out) {
        const int ch = static_cast<int>(setup.spec.chanCount);
        const int rate = static_cast<int>(setup.spec.sampleRate);
        m_pipe = std::make_unique<encoder_pipe>(rate, ch, twinvq::VqfTags{});
    }

    void addChunk(const audio_chunk& chunk, abort_callback& abort) override {
        m_pipe->add_chunk(chunk, abort);
    }

    void finalize(abort_callback& abort) override {
        m_pipe->finalize(m_out, abort);
    }

private:
    file::ptr m_out;
    std::unique_ptr<encoder_pipe> m_pipe;
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

pfc::string8 make_output_path(const char* dest_folder, const char* src_path, t_uint32 subsong) {
    pfc::string8 fn;
    filesystem::get(src_path)->extract_filename_ext(src_path, fn);
    pfc::string8 name = pfc::string_filename(fn);
    if (name.is_empty())
        name = "track";
    if (subsong > 0)
        name << "." << subsong;
    name << ".vqf";
    pfc::string8 dest = dest_folder;
    dest.end_with(filesystem::get(dest)->pathSeparator());
    dest += name;
    return dest;
}

class convert_process : public threaded_process_callback {
public:
    convert_process(metadb_handle_list_cref items, const char* dest)
        : m_items(items), m_dest(dest) {}

    void on_init(ctx_t) override {}

    void run(threaded_process_status& status, abort_callback& abort) override {
        const t_size count = m_items.get_count();
        for (t_size i = 0; i < count; ++i) {
            abort.check();
            const metadb_handle_ptr track = m_items[i];
            status.set_progress(i, count);
            status.set_progress_secondary(0);
            status.set_item_path(track->get_path());
            try {
                encode_one(track, status, abort);
                ++m_ok;
            } catch (exception_aborted) {
                throw;
            } catch (const std::exception& e) {
                m_errors << file_path_display(track->get_path()) << ": " << e << "\n";
            }
        }
    }

    void on_done(ctx_t, bool aborted) override {
        if (aborted)
            return;
        if (!m_errors.is_empty()) {
            popup_message::g_complain("TwinVQ / VQF conversion finished with errors", m_errors);
        } else {
            popup_message::g_show(PFC_string_formatter() << "Encoded " << m_ok << " file(s) to TwinVQ / VQF.",
                                  "TwinVQ encoder");
        }
    }

private:
    void encode_one(metadb_handle_ptr track, threaded_process_status& status, abort_callback& abort) {
        input_decoder::ptr dec;
        input_entry::g_open_for_decoding(dec, nullptr, track->get_path(), abort);
        const t_uint32 subsong = track->get_subsong_index();
        dec->initialize(subsong, input_flag_simpledecode, abort);

        file_info_impl info;
        dec->get_info(subsong, info, abort);
        const double length = info.get_length();
        const twinvq::VqfTags tags = tags_from_info(info);

        audio_chunk_impl chunk;
        std::unique_ptr<encoder_pipe> pipe;
        double decoded = 0;
        while (dec->run(chunk, abort)) {
            if (!pipe) {
                pipe = std::make_unique<encoder_pipe>(static_cast<int>(chunk.get_sample_rate()),
                                                      static_cast<int>(chunk.get_channel_count()), tags);
            }
            pipe->add_chunk(chunk, abort);
            if (length > 0) {
                decoded += chunk.get_duration();
                if (decoded > length)
                    decoded = length;
                status.set_progress_secondary_float(decoded / length);
            }
        }
        if (!pipe)
            throw std::runtime_error("decoder returned no audio");

        pfc::string8 dest_dir = m_dest;
        if (dest_dir.is_empty())
            dest_dir = pfc::string_directory(track->get_path());
        const pfc::string8 dest = make_output_path(dest_dir, track->get_path(), subsong);
        file::ptr out;
        filesystem::g_open_write_new(out, dest, abort);
        pipe->finalize(out, abort);
    }

    const metadb_handle_list m_items;
    const pfc::string8 m_dest;
    t_size m_ok = 0;
    pfc::string_formatter m_errors;
};

struct convert_ui {
    int bitrate = 96;
    bool dest_same = true;
    bool ok = false;
};

INT_PTR CALLBACK convert_dlgproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<convert_ui*>(GetWindowLongPtr(wnd, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<convert_ui*>(lp);
        SetWindowLongPtr(wnd, DWLP_USER, lp);
        int sel = k_bitrate_count - 1;
        for (int i = 0; i < k_bitrate_count; ++i) {
            pfc::string8 label;
            label << k_bitrates[i] << " kbps";
            uSendDlgItemMessageText(wnd, IDC_BITRATE, CB_ADDSTRING, 0, label);
            if (k_bitrates[i] <= st->bitrate)
                sel = i;
        }
        SendDlgItemMessage(wnd, IDC_BITRATE, CB_SETCURSEL, sel, 0);
        CheckRadioButton(wnd, IDC_DEST_SAME, IDC_DEST_ASK, st->dest_same ? IDC_DEST_SAME : IDC_DEST_ASK);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            const LRESULT idx = SendDlgItemMessage(wnd, IDC_BITRATE, CB_GETCURSEL, 0, 0);
            if (idx >= 0 && idx < k_bitrate_count)
                st->bitrate = k_bitrates[static_cast<int>(idx)];
            st->dest_same = IsDlgButtonChecked(wnd, IDC_DEST_SAME) == BST_CHECKED;
            st->ok = true;
            EndDialog(wnd, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(wnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void run_convert(metadb_handle_list_cref items) {
    if (items.get_count() == 0)
        return;
    if (!ModalDialogPrologue())
        return;
    const HWND parent = core_api::get_main_window();

    convert_ui ui;
    ui.bitrate = static_cast<int>(g_bitrate.get());
    ui.dest_same = g_dest_same.get();
    {
        modal_dialog_scope scope(parent);
        DialogBoxParamW(core_api::get_my_instance(), MAKEINTRESOURCEW(IDD_CONVERT), parent, convert_dlgproc,
                        reinterpret_cast<LPARAM>(&ui));
    }
    if (!ui.ok)
        return;

    g_bitrate.set(ui.bitrate);
    g_dest_same.set(ui.dest_same);

    pfc::string8 dest;
    if (!ui.dest_same) {
        pfc::string8 folder;
        {
            modal_dialog_scope scope(parent);
            if (!uBrowseForFolder(parent, "Convert to TwinVQ / VQF", folder))
                return;
        }
        dest = "file://";
        dest += folder;
    }

    auto worker = fb2k::service_new<convert_process>(items, dest);
    const uint32_t flags =
        threaded_process::flag_show_abort | threaded_process::flag_show_progress_dual | threaded_process::flag_show_item;
    threaded_process::get()->run_modeless(worker, flags, parent, "Convert to TwinVQ / VQF");
}

class vqf_convert_item : public contextmenu_item_simple {
public:
    GUID get_parent() override { return contextmenu_groups::convert; }
    double get_sort_priority() override { return -10; }
    unsigned get_num_items() override { return 1; }
    void get_item_name(unsigned, pfc::string_base& out) override { out = "TwinVQ / VQF"; }
    void context_command(unsigned, metadb_handle_list_cref data, const GUID&) override { run_convert(data); }
    GUID get_item_guid(unsigned) override { return guid_convert; }
    bool get_item_description(unsigned, pfc::string_base& out) override {
        out = "Encode the selected tracks to TwinVQ / VQF (native encoder).";
        return true;
    }
};

contextmenu_item_factory_t<vqf_convert_item> g_vqf_convert;

} // namespace
