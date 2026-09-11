#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using std::array;
using std::cout;
using std::ofstream;
using std::string;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;
using std::vector;

static constexpr uint32_t MAGIC_LOG  = 0xAAAA0000;
static constexpr uint32_t MAGIC_DAT  = 0xBBBB0000;
static constexpr uint32_t MAGIC_META = 0xCCCC0000;
static constexpr uint32_t EXPECTED_CHIP_ID = 0x1FFFE;
static constexpr std::size_t MAX_SAMPLES = 256;

namespace etroc {
static constexpr uint64_t SYNC = 0x3C5C;
inline bool is_data(uint64_t d)    { return (d >> 39) & 1U; }
inline bool is_sync(uint64_t d)    { return !is_data(d) && (((d >> 24) & 0x7FFFU) == SYNC); }
inline bool is_header(uint64_t d)  { return is_sync(d) && (((d >> 22) & 0x3U) == 0U); }
inline bool is_filler(uint64_t d)  { return is_sync(d) && (((d >> 22) & 0x3U) == 2U); }
inline bool is_trailer(uint64_t d) { return !is_data(d) && !is_sync(d); }

inline uint32_t hdr_bcid(uint64_t d)   { return d & 0xFFFU; }
inline uint8_t hdr_l1_cnt(uint64_t d)  { return (d >> 14) & 0xFFU; }
inline uint8_t dat_ea(uint64_t d)      { return (d >> 37) & 0x3U; }
inline uint8_t dat_col1(uint64_t d)    { return (d >> 33) & 0xFU; }
inline uint8_t dat_row1(uint64_t d)    { return (d >> 29) & 0xFU; }
inline uint8_t dat_col2(uint64_t d)    { return (d >> 25) & 0xFU; }
inline uint8_t dat_row2(uint64_t d)    { return (d >> 21) & 0xFU; }
inline uint32_t dat_bcid(uint64_t d)   { return (d >> 9) & 0xFFFU; }
inline uint32_t trl_chipid(uint64_t d) { return (d >> 22) & 0x1FFFFU; }
} // namespace etroc

enum class FrameType { Header, Data, Trailer, Filler, Unknown };

FrameType classify(uint64_t w, bool require_expected_chip = true) {
    if (etroc::is_data(w)) return FrameType::Data;
    if (etroc::is_header(w)) return FrameType::Header;
    if (etroc::is_filler(w)) return FrameType::Filler;
    if (etroc::is_trailer(w)) {
        if (!require_expected_chip || etroc::trl_chipid(w) == EXPECTED_CHIP_ID) {
            return FrameType::Trailer;
        }
    }
    return FrameType::Unknown;
}

const char* frame_name(FrameType t) {
    switch (t) {
        case FrameType::Header:  return "HEADER";
        case FrameType::Data:    return "DATA";
        case FrameType::Trailer: return "TRAILER";
        case FrameType::Filler:  return "FILLER";
        default:                 return "UNKNOWN";
    }
}

using HitMap = array<array<uint64_t, 16>, 16>;

struct Sample {
    uint64_t word = 0;
    uint8_t col1 = 0, row1 = 0, col2 = 0, row2 = 0, ea = 0;
    uint32_t data_bcid = 0;
};

struct ChannelStats {
    uint64_t reconstructed = 0;
    uint64_t duplicate_consecutive = 0;
    uint64_t raw_headers = 0, raw_data = 0, raw_trailers = 0, raw_fillers = 0, raw_unknown = 0;
    uint64_t accepted_headers = 0, accepted_data = 0, accepted_trailers = 0, accepted_fillers = 0, accepted_unknown = 0;
    uint64_t fake_trailers = 0;
    uint64_t words_before_header = 0, orphan_data = 0, boundary_cuts = 0, l1_jump_discarded = 0;
    uint64_t committed_data = 0, ea_pass = 0, intra_pass = 0, bcid_pass = 0;
    uint64_t intra_bcid_pass = 0, final_pass = 0;
};

class ChannelAnalyzer {
public:
    ChannelAnalyzer(string mode, string channel)
        : mode_(std::move(mode)), channel_(std::move(channel)) {}

    void process(uint64_t w) {
        ++s_.reconstructed;

        // Pre-cut view: proves what the selected 40-bit reconstruction produces.
        const FrameType raw_t = classify(w, false);
        count_raw(raw_t);
        if (raw_t == FrameType::Data) {
            fill(raw_col1_, etroc::dat_row1(w), etroc::dat_col1(w));
            fill(raw_col2_, etroc::dat_row2(w), etroc::dat_col2(w));
            if (samples_.size() < MAX_SAMPLES) {
                samples_.push_back({w, etroc::dat_col1(w), etroc::dat_row1(w),
                                    etroc::dat_col2(w), etroc::dat_row2(w),
                                    etroc::dat_ea(w), etroc::dat_bcid(w)});
            }
        }

        // Faithful reproduction of the original analyzer's stutter guard.
        if (w == last_word_) {
            ++s_.duplicate_consecutive;
            return;
        }
        last_word_ = w;

        FrameType t = classify(w, true);
        if (etroc::is_trailer(w) && !etroc::is_sync(w) &&
            etroc::trl_chipid(w) != EXPECTED_CHIP_ID) {
            ++s_.fake_trailers;
        }
        count_accepted(t);

        if (!synced_) {
            if (t == FrameType::Header) synced_ = true;
            else {
                ++s_.words_before_header;
                return;
            }
        }

        if (t == FrameType::Data) {
            if (!in_event_) {
                ++s_.orphan_data;
                return;
            }
            current_event_.push_back(w);
            return;
        }

        if (t == FrameType::Header) {
            const uint8_t current_l1 = etroc::hdr_l1_cnt(w);
            bool l1_jump = false;

            if (in_event_) {
                ++s_.boundary_cuts;
                expect_l1_jump_ = true;
            }
            if (!first_event_ && !expect_l1_jump_) {
                const uint8_t expected = static_cast<uint8_t>(last_l1_ + 1U);
                l1_jump = current_l1 != expected;
            }
            first_event_ = false;
            last_l1_ = current_l1;
            expect_l1_jump_ = false;

            if (has_pending_event_) {
                if (l1_jump) ++s_.l1_jump_discarded;
                else commit_pending_event();
                has_pending_event_ = false;
            }

            in_event_ = true;
            current_event_.clear();
            header_bcid_ = etroc::hdr_bcid(w);
            return;
        }

        if (t == FrameType::Trailer) {
            if (in_event_) {
                has_pending_event_ = true;
                in_event_ = false;
            }
            return;
        }
    }

    void finalize() {
        if (has_pending_event_) commit_pending_event();
        if (in_event_) ++s_.boundary_cuts;
    }

    void write(const fs::path& output_dir, const string& stem) const {
        const string prefix = stem + "_" + mode_ + "_" + channel_;
        write_map(output_dir / (prefix + "_raw_col1_map.csv"), raw_col1_);
        write_map(output_dir / (prefix + "_raw_col2_map.csv"), raw_col2_);
        write_map(output_dir / (prefix + "_event_col1_map.csv"), event_col1_);
        write_map(output_dir / (prefix + "_event_col2_map.csv"), event_col2_);
        write_map(output_dir / (prefix + "_intra_pass_map.csv"), intra_map_);
        write_map(output_dir / (prefix + "_bcid_pass_map.csv"), bcid_map_);
        write_map(output_dir / (prefix + "_intra_bcid_pass_map.csv"), intra_bcid_map_);
        write_map(output_dir / (prefix + "_final_ea_pass_map.csv"), final_map_);

        ofstream out(output_dir / (prefix + "_summary.csv"));
        out << "Metric,Value\n";
#define WRITE_STAT(name) out << #name << ',' << s_.name << '\n'
        WRITE_STAT(reconstructed);
        WRITE_STAT(duplicate_consecutive);
        WRITE_STAT(raw_headers); WRITE_STAT(raw_data); WRITE_STAT(raw_trailers);
        WRITE_STAT(raw_fillers); WRITE_STAT(raw_unknown);
        WRITE_STAT(accepted_headers); WRITE_STAT(accepted_data); WRITE_STAT(accepted_trailers);
        WRITE_STAT(accepted_fillers); WRITE_STAT(accepted_unknown); WRITE_STAT(fake_trailers);
        WRITE_STAT(words_before_header); WRITE_STAT(orphan_data); WRITE_STAT(boundary_cuts);
        WRITE_STAT(l1_jump_discarded); WRITE_STAT(committed_data); WRITE_STAT(ea_pass);
        WRITE_STAT(intra_pass); WRITE_STAT(bcid_pass); WRITE_STAT(intra_bcid_pass);
        WRITE_STAT(final_pass);
#undef WRITE_STAT

        ofstream samples(output_dir / (prefix + "_samples.csv"));
        samples << "WordHex,EA,Col1,Row1,Col2,Row2,DataBCID\n";
        for (const auto& x : samples_) {
            samples << hex40(x.word) << ',' << unsigned(x.ea) << ','
                    << unsigned(x.col1) << ',' << unsigned(x.row1) << ','
                    << unsigned(x.col2) << ',' << unsigned(x.row2) << ','
                    << x.data_bcid << '\n';
        }
    }

    const ChannelStats& stats() const { return s_; }

private:
    static void fill(HitMap& map, uint8_t row, uint8_t col) {
        if (row < 16 && col < 16) ++map[row][col];
    }

    static void write_map(const fs::path& path, const HitMap& map) {
        ofstream out(path);
        for (std::size_t r = 0; r < 16; ++r) {
            for (std::size_t c = 0; c < 16; ++c) {
                if (c) out << ',';
                out << map[r][c];
            }
            out << '\n';
        }
    }

    static string hex40(uint64_t w) {
        std::ostringstream ss;
        ss << "0x" << std::hex << std::uppercase << std::setw(10)
           << std::setfill('0') << (w & 0xFFFFFFFFFFULL);
        return ss.str();
    }

    void count_raw(FrameType t) {
        switch (t) {
            case FrameType::Header:  ++s_.raw_headers; break;
            case FrameType::Data:    ++s_.raw_data; break;
            case FrameType::Trailer: ++s_.raw_trailers; break;
            case FrameType::Filler:  ++s_.raw_fillers; break;
            default:                 ++s_.raw_unknown; break;
        }
    }

    void count_accepted(FrameType t) {
        switch (t) {
            case FrameType::Header:  ++s_.accepted_headers; break;
            case FrameType::Data:    ++s_.accepted_data; break;
            case FrameType::Trailer: ++s_.accepted_trailers; break;
            case FrameType::Filler:  ++s_.accepted_fillers; break;
            default:                 ++s_.accepted_unknown; break;
        }
    }

    void commit_pending_event() {
        for (uint64_t w : current_event_) {
            ++s_.committed_data;
            const uint8_t row1 = etroc::dat_row1(w), col1 = etroc::dat_col1(w);
            const uint8_t row2 = etroc::dat_row2(w), col2 = etroc::dat_col2(w);
            const bool ea_ok = etroc::dat_ea(w) < 2;
            const bool intra_ok = row1 == row2 && col1 == col2;
            const bool bcid_ok = etroc::dat_bcid(w) == header_bcid_;

            fill(event_col1_, row1, col1);
            fill(event_col2_, row2, col2);
            if (ea_ok) ++s_.ea_pass;
            if (intra_ok) {
                ++s_.intra_pass;
                fill(intra_map_, row1, col1);
            }
            if (bcid_ok) {
                ++s_.bcid_pass;
                fill(bcid_map_, row1, col1);
            }
            if (intra_ok && bcid_ok) {
                ++s_.intra_bcid_pass;
                fill(intra_bcid_map_, row1, col1);
            }
            if (intra_ok && bcid_ok && ea_ok) {
                ++s_.final_pass;
                fill(final_map_, row1, col1);
            }
        }
    }

    string mode_, channel_;
    ChannelStats s_;
    HitMap raw_col1_{}, raw_col2_{}, event_col1_{}, event_col2_{};
    HitMap intra_map_{}, bcid_map_{}, intra_bcid_map_{}, final_map_{};
    vector<Sample> samples_;
    uint64_t last_word_ = 0xFFFFFFFFFFFFFFFFULL;
    bool synced_ = false, expect_l1_jump_ = false, in_event_ = false;
    bool has_pending_event_ = false, first_event_ = true;
    uint8_t last_l1_ = 0;
    uint32_t header_bcid_ = 0;
    vector<uint64_t> current_event_;
};

struct MarkerStats {
    uint64_t zero_words = 0, non_payload_words = 0;
    uint64_t marker_100 = 0, marker_101 = 0, marker_110 = 0, marker_111 = 0;
    uint64_t right_pairs = 0, left_pairs = 0;
    uint64_t right_high_without_low = 0, left_high_without_low = 0;
    uint64_t right_low_overwritten = 0, left_low_overwritten = 0;
    uint64_t right_low_without_high = 0, left_low_without_high = 0;
    uint64_t unknown_marker = 0;
    uint64_t hw_reported_drops = 0, salvaged_words = 0;
    uint64_t injected_words = 0, failed_recovery_words = 0;
};

struct HWDrop {
    uint32_t addr = 0;
    uint32_t count = 0;
    int lap = -1;
    vector<uint32_t> salvaged_payloads;
};

struct PairSample {
    string channel;
    uint32_t low = 0, high = 0;
    uint64_t legacy = 0, vhdl = 0;
};

string hex32(uint32_t w) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << std::setw(8)
       << std::setfill('0') << w;
    return ss.str();
}

string hex40(uint64_t w) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << std::setw(10)
       << std::setfill('0') << (w & 0xFFFFFFFFFFULL);
    return ss.str();
}

uint64_t reconstruct_legacy24_16(uint32_t low, uint32_t high) {
    return (static_cast<uint64_t>(high & 0xFFFFU) << 24) | (low & 0xFFFFFFU);
}

uint64_t reconstruct_vhdl29_11(uint32_t low, uint32_t high) {
    return (static_cast<uint64_t>(high & 0x7FFU) << 29) | (low & 0x1FFFFFFFU);
}

struct Options {
    fs::path input = "daq_bin_results";
    fs::path output = "pseudo_diag_results";
};

Options parse_options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) o.input = argv[++i];
        else if (arg == "--output" && i + 1 < argc) o.output = argv[++i];
        else if (arg == "--help") {
            cout << "Usage: " << argv[0]
                 << " [--input daq_bin_results] [--output pseudo_diag_results]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown or incomplete argument: " + arg);
        }
    }
    return o;
}

void write_marker_summary(const fs::path& path, const MarkerStats& s) {
    ofstream out(path);
    out << "Metric,Value\n";
#define WRITE_MARKER(name) out << #name << ',' << s.name << '\n'
    WRITE_MARKER(zero_words); WRITE_MARKER(non_payload_words);
    WRITE_MARKER(marker_100); WRITE_MARKER(marker_101);
    WRITE_MARKER(marker_110); WRITE_MARKER(marker_111);
    WRITE_MARKER(right_pairs); WRITE_MARKER(left_pairs);
    WRITE_MARKER(right_high_without_low); WRITE_MARKER(left_high_without_low);
    WRITE_MARKER(right_low_overwritten); WRITE_MARKER(left_low_overwritten);
    WRITE_MARKER(right_low_without_high); WRITE_MARKER(left_low_without_high);
    WRITE_MARKER(unknown_marker);
    WRITE_MARKER(hw_reported_drops); WRITE_MARKER(salvaged_words);
    WRITE_MARKER(injected_words); WRITE_MARKER(failed_recovery_words);
#undef WRITE_MARKER
}

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (!fs::is_directory(options.input)) {
            std::cerr << "Input directory not found: " << options.input << '\n';
            return 1;
        }
        fs::create_directories(options.output);

        vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(options.input)) {
            if (entry.is_regular_file() && entry.path().extension() == ".bin") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        if (files.empty()) {
            std::cerr << "No .bin files found in " << options.input << '\n';
            return 1;
        }

        for (const fs::path& file : files) {
            const string stem = file.stem().string();
            cout << "Analyzing " << file.filename().string() << "\n";

            ChannelAnalyzer legacy_right("legacy24_16", "RIGHT");
            ChannelAnalyzer legacy_left("legacy24_16", "LEFT");
            ChannelAnalyzer vhdl_right("vhdl29_11", "RIGHT");
            ChannelAnalyzer vhdl_left("vhdl29_11", "LEFT");
            MarkerStats marker_stats;
            vector<PairSample> pair_samples;
            vector<HWDrop> pending_drops;
            bool first_log_skipped = false;
            uint64_t global_valid_count = 0;

            uint32_t right_low = 0, left_low = 0;
            bool right_ready = false, left_ready = false;

            auto process_dpram_word = [&](uint32_t w) {
                if (w == 0) {
                    ++marker_stats.zero_words;
                    return;
                }
                if ((w >> 31) == 0) {
                    ++marker_stats.non_payload_words;
                    return;
                }

                const uint8_t marker = (w >> 29) & 0x7U;
                switch (marker) {
                    case 0b100:
                        ++marker_stats.marker_100;
                        if (right_ready) ++marker_stats.right_low_overwritten;
                        right_low = w;
                        right_ready = true;
                        break;
                    case 0b101:
                        ++marker_stats.marker_101;
                        if (!right_ready) {
                            ++marker_stats.right_high_without_low;
                            break;
                        }
                        ++marker_stats.right_pairs;
                        {
                            const uint64_t legacy = reconstruct_legacy24_16(right_low, w);
                            const uint64_t vhdl = reconstruct_vhdl29_11(right_low, w);
                            legacy_right.process(legacy);
                            vhdl_right.process(vhdl);
                            if (pair_samples.size() < MAX_SAMPLES) {
                                pair_samples.push_back({"RIGHT", right_low, w, legacy, vhdl});
                            }
                        }
                        right_ready = false;
                        break;
                    case 0b110:
                        ++marker_stats.marker_110;
                        if (left_ready) ++marker_stats.left_low_overwritten;
                        left_low = w;
                        left_ready = true;
                        break;
                    case 0b111:
                        ++marker_stats.marker_111;
                        if (!left_ready) {
                            ++marker_stats.left_high_without_low;
                            break;
                        }
                        ++marker_stats.left_pairs;
                        {
                            const uint64_t legacy = reconstruct_legacy24_16(left_low, w);
                            const uint64_t vhdl = reconstruct_vhdl29_11(left_low, w);
                            legacy_left.process(legacy);
                            vhdl_left.process(vhdl);
                            if (pair_samples.size() < MAX_SAMPLES) {
                                pair_samples.push_back({"LEFT", left_low, w, legacy, vhdl});
                            }
                        }
                        left_ready = false;
                        break;
                    default:
                        ++marker_stats.unknown_marker;
                        break;
                }
            };

            std::ifstream in(file, std::ios::binary);
            uint32_t magic = 0, size_words = 0;
            while (in.read(reinterpret_cast<char*>(&magic), sizeof(magic)) &&
                   in.read(reinterpret_cast<char*>(&size_words), sizeof(size_words))) {
                vector<uint32_t> block(size_words);
                if (!in.read(reinterpret_cast<char*>(block.data()),
                             static_cast<std::streamsize>(size_words) * sizeof(uint32_t))) {
                    std::cerr << "Truncated block in " << file.filename().string() << '\n';
                    break;
                }
                if (magic == MAGIC_LOG) {
                    if (block.empty() || block[0] == 0xDEADBEEFU) continue;
                    // Match the recovery behavior in test_bandwidth_reco.cxx.
                    if (!first_log_skipped) {
                        first_log_skipped = true;
                        continue;
                    }

                    constexpr uint32_t ADDR_WIDTH = 12;
                    constexpr uint32_t ADDR_MASK = (1U << ADDR_WIDTH) - 1U;
                    const uint32_t valid_count = block[0] & ADDR_MASK;
                    const uint32_t limit = std::min<uint32_t>(block.size(), valid_count + 1U);
                    vector<uint32_t> temporary_payloads;

                    for (uint32_t i = 1; i < limit; ++i) {
                        const uint32_t log_word = block[i];
                        if ((log_word >> 31) == 1U) {
                            temporary_payloads.push_back(log_word);
                        } else if ((log_word >> 30) == 0U) {
                            const uint32_t drops = (log_word >> 18) & 0xFFFU;
                            const uint32_t addr = log_word & 0x3FFFFU;
                            marker_stats.hw_reported_drops += drops;
                            marker_stats.salvaged_words += temporary_payloads.size();
                            pending_drops.push_back({addr, drops, -1, temporary_payloads});
                            temporary_payloads.clear();
                        } else {
                            const int lap = (log_word & 0x7U) == 0U
                                                ? 7
                                                : static_cast<int>((log_word & 0x7U) - 1U);
                            for (auto& drop : pending_drops) {
                                if (drop.lap == -1) drop.lap = lap;
                            }
                        }
                    }
                    const bool overflowed = ((block[0] >> 29) & 1U) == 1U;
                    if (overflowed) temporary_payloads.clear();
                } else if (magic == MAGIC_DAT) {
                    for (uint32_t w : block) {
                        const uint32_t physical_addr = global_valid_count % 262144U;
                        const int current_lap = static_cast<int>((global_valid_count / 262144U) % 8U);
                        ++global_valid_count;

                        for (auto it = pending_drops.begin(); it != pending_drops.end();) {
                            if (it->addr == physical_addr &&
                                (it->lap == current_lap || it->lap == -1)) {
                                marker_stats.injected_words += it->salvaged_payloads.size();
                                for (uint32_t recovered : it->salvaged_payloads) {
                                    process_dpram_word(recovered);
                                }
                                it = pending_drops.erase(it);
                                break;
                            }
                            ++it;
                        }
                        process_dpram_word(w);
                    }
                } else if (magic != MAGIC_META) {
                    std::cerr << "Unknown block magic " << hex32(magic) << " in "
                              << file.filename().string() << '\n';
                    break;
                }
            }

            if (right_ready) ++marker_stats.right_low_without_high;
            if (left_ready) ++marker_stats.left_low_without_high;
            for (const auto& drop : pending_drops) {
                marker_stats.failed_recovery_words += drop.salvaged_payloads.size();
            }

            legacy_right.finalize(); legacy_left.finalize();
            vhdl_right.finalize(); vhdl_left.finalize();

            const fs::path file_out = options.output / stem;
            fs::create_directories(file_out);
            legacy_right.write(file_out, stem); legacy_left.write(file_out, stem);
            vhdl_right.write(file_out, stem); vhdl_left.write(file_out, stem);
            write_marker_summary(file_out / (stem + "_marker_summary.csv"), marker_stats);

            ofstream pairs(file_out / (stem + "_reconstruction_samples.csv"));
            pairs << "Channel,Low32,High32,Legacy24_16,LegacyType,VHDL29_11,VHDLType\n";
            for (const auto& x : pair_samples) {
                pairs << x.channel << ',' << hex32(x.low) << ',' << hex32(x.high) << ','
                      << hex40(x.legacy) << ',' << frame_name(classify(x.legacy, false)) << ','
                      << hex40(x.vhdl) << ',' << frame_name(classify(x.vhdl, false)) << '\n';
            }

            cout << "  output: " << file_out.string() << '\n';
            cout << "  RIGHT final-pass legacy=" << legacy_right.stats().final_pass
                 << ", vhdl=" << vhdl_right.stats().final_pass << '\n';
            cout << "  LEFT  final-pass legacy=" << legacy_left.stats().final_pass
                 << ", vhdl=" << vhdl_left.stats().final_pass << '\n';
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
