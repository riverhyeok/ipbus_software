#include <iostream>
#include <fstream>
#include <map>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <cmath>
#include <filesystem>

// ROOT 라이브러리 포함
#include <TApplication.h>
#include <TROOT.h>
#include <TH1D.h>
#include <TCanvas.h>
#include <TStyle.h>
#include <TError.h>

namespace fs = std::filesystem;
using namespace std;

// =====================================================================
// 1. Data Structures & ETROC2 Bit Masking
// =====================================================================
const uint32_t MAGIC_LOG  = 0xAAAA0000;
const uint32_t MAGIC_DAT  = 0xBBBB0000;
const uint32_t MAGIC_META = 0xCCCC0000;
const double T3_NS = 3.125; 

namespace etroc {
    static const uint64_t SYNC = 0x3C5C;
    inline bool is_data(uint64_t d)    { return  (d >> 39) & 1; }
    inline bool is_sync(uint64_t d)    { return !is_data(d) && (((d >> 24) & 0x7FFF) == SYNC); }
    inline bool is_header(uint64_t d)  { return  is_sync(d) && (((d >> 22) & 0x3) == 0x0); }
    inline bool is_filler(uint64_t d)  { return  is_sync(d) && (((d >> 22) & 0x3) == 0x2); }
    inline bool is_trailer(uint64_t d) { return !is_data(d) && !is_sync(d); }
    
    inline uint32_t hdr_bcid(uint64_t d)   { return  d        & 0xFFF; }
    inline uint8_t  hdr_type(uint64_t d)   { return (d >> 12) & 0x3;   }
    inline uint8_t  hdr_l1_cnt(uint64_t d) { return (d >> 14) & 0xFF;  }
    
    inline uint8_t  dat_ea(uint64_t d)     { return (d >> 37) & 0x3;   }
    inline uint8_t  dat_col(uint64_t d)    { return (d >> 33) & 0xF;   }
    inline uint8_t  dat_row(uint64_t d)    { return (d >> 29) & 0xF;   }
    inline uint16_t dat_toa(uint64_t d)    { return (d >> 19) & 0x3FF; }
    inline uint16_t dat_tot(uint64_t d)    { return (d >> 10) & 0x1FF; }
    inline uint16_t dat_cal(uint64_t d)    { return d & 0x3FF;         }
    inline uint16_t dat_counter_a(uint64_t d){ return d & 0x1FF;       }
    
    inline uint32_t trl_chipid(uint64_t d) { return (d >> 22) & 0x1FFFF;}
    inline uint8_t  trl_status(uint64_t d) { return (d >> 16) & 0x3F;   }
    inline uint16_t trl_hits(uint64_t d)   { return (d >>  8) & 0xFF;   }

    uint8_t calculate_crc8(const std::vector<uint8_t>& data_bytes) {
        uint8_t crc = 0x00;
        for (uint8_t b : data_bytes) {
            crc ^= b;
            for (int i = 0; i < 8; i++) {
                if (crc & 0x80) crc = (crc << 1) ^ 0x2F;
                else            crc <<= 1;
            }
        }
        return crc;
    }
}

enum FrameType { HEADER = 0, DATA = 1, TRAILER = 2, FILLER = 3, UNKNOWN = 4 };
FrameType get_payload_type(uint64_t w) {
    if (etroc::is_data(w))    return DATA;
    if (etroc::is_header(w))  return HEADER;
    if (etroc::is_filler(w))  return FILLER;
    if (etroc::is_trailer(w)) return TRAILER;
    return UNKNOWN;
}

struct HWDrop {
    uint32_t addr; uint32_t count; int lap;
    vector<uint32_t> salvaged_payloads;
};

struct SystemStats {
    uint64_t total_32b_words = 0, total_ram_buffer_empty = 0;
    double elapsed_sec = 0.0, bandwidth_mbps = 0.0, effective_bandwidth_mbps = 0.0;
    uint64_t hw_reported_drops = 0, total_salvaged_words = 0, words_injected = 0;
    uint64_t fail_address_drift_orphan = 0, fail_word_order_quad_slip = 0;
    void add(const SystemStats& o) {
        total_32b_words += o.total_32b_words; total_ram_buffer_empty += o.total_ram_buffer_empty;
    }
};

struct RecoStats {
    uint64_t total_frames = 0, headers = 0, trailers = 0, fillers = 0, hits_raw = 0, valid_hits = 0;
    uint64_t events_missing_header = 0, orphan_data_words = 0, words_dropped_at_start = 0;
    uint64_t wp_boundary_cuts = 0, weird_wp_boundary_cuts = 0;
    uint64_t events_perfect_hits = 0, events_hit_mismatch = 0, events_corrupted_bcid = 0, events_discarded_by_jump = 0;
    uint64_t l1_counter_jumps = 0, l1_jump_sum = 0, l1_jump_sq_sum = 0;
    uint64_t counter_a_jumps = 0, counter_a_jump_sum = 0, counter_a_jump_sq_sum = 0;
    uint64_t ea_counts[4] = {0}, fatal_ea_errors = 0, expected_total_hits = 0, sw_lost_hits = 0;
    uint64_t header_type_counts[4] = {0}, status_counts[64] = {0}, l1_buffer_status_counts[4] = {0};
    uint64_t hit_map[16][16] = {0};
    uint64_t fake_trailers_blocked = 0, unknown_frames = 0, seu_error_counts = 0;
    uint64_t crc_match = 0, crc_mismatch = 0;
    uint64_t crc_err_ea = 0, crc_err_missing = 0, crc_err_missing_with_ea = 0, crc_err_silent = 0;
    std::map<uint32_t, uint64_t> chip_id_counts;
    
    void add(const RecoStats& o) {
        total_frames += o.total_frames; headers += o.headers; trailers += o.trailers;
        fillers += o.fillers; hits_raw += o.hits_raw; valid_hits += o.valid_hits;
        events_missing_header += o.events_missing_header; orphan_data_words += o.orphan_data_words;
        words_dropped_at_start += o.words_dropped_at_start; wp_boundary_cuts += o.wp_boundary_cuts;
        events_perfect_hits += o.events_perfect_hits; events_hit_mismatch += o.events_hit_mismatch;
        events_corrupted_bcid += o.events_corrupted_bcid; events_discarded_by_jump += o.events_discarded_by_jump;
        l1_counter_jumps += o.l1_counter_jumps; l1_jump_sum += o.l1_jump_sum; l1_jump_sq_sum += o.l1_jump_sq_sum;
        counter_a_jumps += o.counter_a_jumps; counter_a_jump_sum += o.counter_a_jump_sum; counter_a_jump_sq_sum += o.counter_a_jump_sq_sum;
        for(int i=0; i<4; i++) { ea_counts[i] += o.ea_counts[i]; header_type_counts[i] += o.header_type_counts[i]; l1_buffer_status_counts[i] += o.l1_buffer_status_counts[i]; }
        for(int i=0; i<64; i++) status_counts[i] += o.status_counts[i];
        for(auto& kv : o.chip_id_counts) chip_id_counts[kv.first] += kv.second;
        seu_error_counts += o.seu_error_counts; fatal_ea_errors += o.fatal_ea_errors;
        crc_match += o.crc_match; crc_mismatch += o.crc_mismatch;
        expected_total_hits += o.expected_total_hits; sw_lost_hits += o.sw_lost_hits;
        for(int r=0; r<16; r++) for(int c=0; c<16; c++) hit_map[r][c] += o.hit_map[r][c];
        fake_trailers_blocked += o.fake_trailers_blocked; unknown_frames += o.unknown_frames;
        crc_err_ea += o.crc_err_ea; crc_err_missing += o.crc_err_missing; crc_err_missing_with_ea += o.crc_err_missing_with_ea; crc_err_silent += o.crc_err_silent;
    }
};

// Global 데이터 보관소 (chan_tag: 0=RIGHT, 1=LEFT)
std::map<uint8_t, TH1D*> g_toa, g_tot, g_cal, g_toa_ns, g_tot_ns, g_status, g_crc, g_bcid_delta, g_hits_per_event;
std::map<uint8_t, RecoStats> g_stats;
std::map<uint8_t, SystemStats> g_sys;

// =====================================================================
// 2. Analyzer Class
// =====================================================================
class EtrocChannelAnalyzer {
    string name_, file_prefix_;
    uint32_t expected_chip_id_;
    RecoStats s_;
    uint8_t chan_tag_;
    
    TH1D *h_toa_, *h_tot_, *h_cal_, *h_toa_ns_, *h_tot_ns_, *h_status_, *h_crc_, *h_bcid_delta_, *h_hits_per_event_;
    
    bool is_synced_ = false, expect_l1_jump_ = false, in_event_ = false, has_pending_event_ = false;
    uint64_t pending_trailer_w_ = 0, event_id_ = 0, last_word_ = 0xFFFFFFFFFFFFFFFFULL;
    uint32_t hdr_bcid_ = 0, last_bcid_ = 0;
    uint8_t  hdr_type_ = 0, last_l1_cnt_ = 0;
    bool     is_first_event_ = true;
    
    std::vector<uint64_t> current_event_data_;
    std::vector<uint8_t> event_bytes_;
    std::vector<string> hit_stream_buffer_;
    
    bool     first_counter_a_[16][16];
    uint16_t last_counter_a_[16][16];

    void push_word_bytes(std::vector<uint8_t>& target_vec, uint64_t w) {
        for (int i = 4; i >= 0; i--) target_vec.push_back((w >> (i * 8)) & 0xFF);
    }

    void commit_pending_event() {
        s_.header_type_counts[hdr_type_]++;
        uint32_t chip_id = etroc::trl_chipid(pending_trailer_w_);
        uint16_t trl_h = etroc::trl_hits(pending_trailer_w_);
        uint8_t status = etroc::trl_status(pending_trailer_w_);
        
        s_.chip_id_counts[chip_id]++;
        if (status < 64) s_.status_counts[status]++;
        s_.l1_buffer_status_counts[(status >> 4) & 0x3]++;
        if ((status >> 3) & 0x1) s_.seu_error_counts++;
        
        h_status_->Fill(status); g_status[chan_tag_]->Fill(status);
        uint64_t event_hits = current_event_data_.size();
        h_hits_per_event_->Fill(event_hits); g_hits_per_event[chan_tag_]->Fill(event_hits);
        if (trl_h == 0 && event_hits >= 200) trl_h = 256;
        
        bool current_event_has_ea_error = false;
        for (uint64_t hit_w : current_event_data_) {
            uint8_t ea = etroc::dat_ea(hit_w);
            s_.ea_counts[ea & 0x3]++;
            if (ea >= 0x2) { s_.fatal_ea_errors++; current_event_has_ea_error = true; continue; }
            
            uint8_t col = etroc::dat_col(hit_w), row = etroc::dat_row(hit_w);
            uint16_t cnt_a = etroc::dat_counter_a(hit_w);
            
            if (first_counter_a_[row][col]) {
                first_counter_a_[row][col] = false;
            } else {
                uint16_t expected_cnt = (last_counter_a_[row][col] + 1) & 0x1FF;
                if (cnt_a != expected_cnt) {
                    uint64_t j_size = (cnt_a - expected_cnt) & 0x1FF;
                    s_.counter_a_jumps++; s_.counter_a_jump_sum += j_size; s_.counter_a_jump_sq_sum += (j_size * j_size);
                }
            }
            last_counter_a_[row][col] = cnt_a;
            s_.valid_hits++; s_.hit_map[row][col]++;
            
            uint16_t tot_code = etroc::dat_tot(hit_w), toa_code = etroc::dat_toa(hit_w), cal_code = etroc::dat_cal(hit_w);
            double toa_ns = 0.0, tot_ns = 0.0;
            if (cal_code > 0) {
                h_toa_->Fill(toa_code); g_toa[chan_tag_]->Fill(toa_code);
                h_tot_->Fill(tot_code); g_tot[chan_tag_]->Fill(tot_code);
                double t_bin = T3_NS / (double)cal_code;
                toa_ns = t_bin * (double)toa_code; tot_ns = t_bin * ((2.0 * tot_code) - std::floor(tot_code / 32.0));
                h_toa_ns_->Fill(toa_ns); g_toa_ns[chan_tag_]->Fill(toa_ns);
                h_tot_ns_->Fill(tot_ns); g_tot_ns[chan_tag_]->Fill(tot_ns);
            }
            h_cal_->Fill(cal_code); g_cal[chan_tag_]->Fill(cal_code);
            hit_stream_buffer_.push_back(to_string(event_id_) + "," + to_string(row) + "," + to_string(col) + "," + to_string(toa_ns) + "," + to_string(tot_ns) + "," + to_string(cal_code) + "," + to_string(ea));
        }
        
        push_word_bytes(event_bytes_, pending_trailer_w_);
        uint8_t crc_syndrome = etroc::calculate_crc8(event_bytes_);
        h_crc_->Fill(crc_syndrome); g_crc[chan_tag_]->Fill(crc_syndrome);
        
        bool is_crc_mismatch = (crc_syndrome != 0x00);
        s_.expected_total_hits += trl_h;
        bool has_missing_hits = (trl_h > event_hits);
        if (has_missing_hits) { s_.events_hit_mismatch++; s_.sw_lost_hits += (trl_h - event_hits); } 
        else s_.events_perfect_hits++;
        
        if (!is_crc_mismatch) s_.crc_match++;
        else {
            s_.crc_mismatch++;
            bool is_silent = true;
            if (current_event_has_ea_error) { s_.crc_err_ea++; is_silent = false; }
            if (has_missing_hits) { s_.crc_err_missing++; is_silent = false; }
            if (has_missing_hits && current_event_has_ea_error) s_.crc_err_missing_with_ea++;
            if (is_silent) s_.crc_err_silent++;
        }
        event_id_++;
    }

public:
    EtrocChannelAnalyzer(const string& name, uint32_t chip_id, const string& prefix, uint8_t chan_tag) 
        : name_(name), expected_chip_id_(chip_id), file_prefix_(prefix), chan_tag_(chan_tag) 
    {
        memset(first_counter_a_, true, sizeof(first_counter_a_));
        memset(last_counter_a_, 0, sizeof(last_counter_a_));
        
        string t = name_ + "_" + file_prefix_;
        h_toa_ = new TH1D(Form("h_toa_%s", t.c_str()), Form("TOA_CODE (%s);TOA_CODE;Counts", name_.c_str()), 1024, 0, 1024);
        h_tot_ = new TH1D(Form("h_tot_%s", t.c_str()), Form("TOT_CODE (%s);TOT_CODE;Counts", name_.c_str()), 512, 0, 512);
        h_cal_ = new TH1D(Form("h_cal_%s", t.c_str()), Form("CAL_CODE (%s);CAL_CODE;Counts", name_.c_str()), 1024, 0, 1024);
        h_toa_ns_ = new TH1D(Form("h_toa_ns_%s", t.c_str()), Form("TOA Time (%s);TOA (ns);Counts", name_.c_str()), 250, 0, 25);
        h_tot_ns_ = new TH1D(Form("h_tot_ns_%s", t.c_str()), Form("TOT Time (%s);TOT (ns);Counts", name_.c_str()), 250, 0, 25);
        h_status_ = new TH1D(Form("h_stat_%s", t.c_str()), Form("Trailer Status Code (%s);Status Code [0-63];Events", name_.c_str()), 64, 0, 64);
        h_crc_    = new TH1D(Form("h_crc_%s", t.c_str()), Form("CRC Syndrome (%s);CRC Result (0=Match);Events", name_.c_str()), 256, 0, 256);
        h_bcid_delta_ = new TH1D(Form("h_bcid_d_%s", t.c_str()), Form("Delta BCID (%s);#Delta BCID;Events", name_.c_str()), 4096, 0, 4096);
        h_hits_per_event_ = new TH1D(Form("h_hpe_%s", t.c_str()), Form("Data Words per Header (%s);Hits;Events", name_.c_str()), 257, -0.5, 256.5);
    }
    
    ~EtrocChannelAnalyzer() {
        delete h_toa_; delete h_tot_; delete h_cal_; delete h_toa_ns_; delete h_tot_ns_;
        delete h_status_; delete h_crc_; delete h_bcid_delta_; delete h_hits_per_event_;
    }

    void process_word(uint64_t w) {
        // 🌟 [Stutter 버그 픽스] 
        if (w == last_word_) return;
        last_word_ = w;

        s_.total_frames++;
        FrameType payload_t = get_payload_type(w);
        
        if (payload_t == TRAILER && etroc::trl_chipid(w) != expected_chip_id_) {
            s_.fake_trailers_blocked++; payload_t = UNKNOWN;
        }
        if (payload_t == UNKNOWN) s_.unknown_frames++;
        
        if (!is_synced_) {
            if (payload_t == HEADER) is_synced_ = true;
            else { s_.words_dropped_at_start++; return; }
        }
        
        if (payload_t == DATA) {
            s_.hits_raw++;
            if (!in_event_) { s_.orphan_data_words++; return; }
            current_event_data_.push_back(w);
            push_word_bytes(event_bytes_, w);
            return;
        }
        
        if (payload_t == HEADER) {
            s_.headers++;
            uint8_t curr_l1_cnt = etroc::hdr_l1_cnt(w);
            uint32_t curr_bcid  = etroc::hdr_bcid(w);
            bool l1_jump_detected = false;
            
            if (in_event_) { s_.wp_boundary_cuts++; expect_l1_jump_ = true; }
            if (!is_first_event_ && !expect_l1_jump_) {
                uint8_t std_cnt = (last_l1_cnt_ + 1) & 0xFF;
                if (curr_l1_cnt != std_cnt) {
                    uint64_t j_size = (curr_l1_cnt - last_l1_cnt_) & 0xFF;
                    s_.l1_counter_jumps++; s_.l1_jump_sum += j_size; s_.l1_jump_sq_sum += (j_size * j_size);
                    l1_jump_detected = true;
                }
                int delta_bcid = (curr_bcid - last_bcid_) & 0xFFF;
                h_bcid_delta_->Fill(delta_bcid); g_bcid_delta[chan_tag_]->Fill(delta_bcid);
            }
            is_first_event_ = false; last_l1_cnt_ = curr_l1_cnt; last_bcid_ = curr_bcid; expect_l1_jump_ = false;
            
            if (has_pending_event_) {
                if (l1_jump_detected) s_.events_discarded_by_jump++;
                else commit_pending_event();
                has_pending_event_ = false;
            }
            
            in_event_ = true; current_event_data_.clear(); event_bytes_.clear();
            push_word_bytes(event_bytes_, w);
            hdr_bcid_ = curr_bcid; hdr_type_ = etroc::hdr_type(w);
            return;
        }
        
        if (payload_t == TRAILER) {
            s_.trailers++;
            if (!in_event_) { s_.events_missing_header++; return; }
            pending_trailer_w_ = w; has_pending_event_ = true; in_event_ = false;
            return;
        }
        if (payload_t == FILLER) s_.fillers++;
    }

    void finalize_stream(SystemStats& sys) {
        if (has_pending_event_) commit_pending_event();
        if (in_event_) s_.wp_boundary_cuts++;
        g_stats[chan_tag_].add(s_);
        g_sys[chan_tag_].add(sys);
    }

    void export_local_plots_and_data() const {
        string out_path = "results/" + file_prefix_ + "_" + name_;
        
        ofstream hsf(out_path + "_hit_stream.csv");
        hsf << "EventID,Row,Col,TOA_ns,TOT_ns,CAL_Code,EA_Code\n";
        for (const auto& str : hit_stream_buffer_) hsf << str << "\n";
        hsf.close();
        
        ofstream hmf(out_path + "_heatmap.csv");
        for (int r = 0; r < 16; r++) {
            for (int c = 0; c < 16; c++) hmf << s_.hit_map[r][c] << (c == 15 ? "" : ",");
            hmf << "\n";
        }
        hmf.close();

        TCanvas c1("c1", "Local", 800, 600);
        if (s_.valid_hits > 0) {
            h_toa_->SetFillColor(38); h_toa_->Draw(); c1.SaveAs((out_path + "_TOA_CODE.png").c_str());
            h_tot_->SetFillColor(46); h_tot_->Draw(); c1.SaveAs((out_path + "_TOT_CODE.png").c_str());
            h_cal_->SetFillColor(30); h_cal_->Draw(); c1.SaveAs((out_path + "_CAL_CODE.png").c_str());
            h_toa_ns_->SetFillColor(38); h_toa_ns_->Draw(); c1.SaveAs((out_path + "_TOA_ns.png").c_str());
            h_tot_ns_->SetFillColor(46); h_tot_ns_->Draw(); c1.SaveAs((out_path + "_TOT_ns.png").c_str());
        }
        if (h_status_->GetEntries() > 0) { h_status_->SetFillColor(41); h_status_->Draw(); c1.SaveAs((out_path + "_Status_Code.png").c_str()); }
        if (h_crc_->GetEntries() > 0) { h_crc_->SetFillColor(42); h_crc_->Draw(); c1.SaveAs((out_path + "_CRC_Syndrome.png").c_str()); }
        if (h_bcid_delta_->GetEntries() > 0) {
            h_bcid_delta_->SetFillColor(43);
            h_bcid_delta_->GetXaxis()->SetRange(max(1, h_bcid_delta_->FindFirstBinAbove(0) - 5), min(4096, h_bcid_delta_->FindLastBinAbove(0) + 5));
            h_bcid_delta_->Draw(); c1.SaveAs((out_path + "_BCID_Delta.png").c_str());
        }
        if (h_hits_per_event_->GetEntries() > 0) {
            h_hits_per_event_->SetFillColor(44);
            h_hits_per_event_->GetXaxis()->SetRange(1, min(257, h_hits_per_event_->FindLastBinAbove(0) + 5));
            h_hits_per_event_->Draw(); c1.SaveAs((out_path + "_Hits_Per_Event.png").c_str());
        }
        
        // Stats CSV (Python 호환용)
        double l1_jump_avg = 0.0, l1_jump_std = 0.0;
        if (s_.l1_counter_jumps > 0) {
            l1_jump_avg = (double)s_.l1_jump_sum / s_.l1_counter_jumps;
            double var = ((double)s_.l1_jump_sq_sum / s_.l1_counter_jumps) - (l1_jump_avg * l1_jump_avg);
            l1_jump_std = var > 0 ? sqrt(var) : 0.0;
        }
        
        double cnt_a_jump_avg = 0.0, cnt_a_jump_std = 0.0;
        if (s_.counter_a_jumps > 0) {
            cnt_a_jump_avg = (double)s_.counter_a_jump_sum / s_.counter_a_jumps;
            double var = ((double)s_.counter_a_jump_sq_sum / s_.counter_a_jumps) - (cnt_a_jump_avg * cnt_a_jump_avg);
            cnt_a_jump_std = var > 0 ? sqrt(var) : 0.0;
        }

        uint64_t total_complete = s_.events_perfect_hits + s_.events_hit_mismatch + s_.events_corrupted_bcid + s_.events_discarded_by_jump;
        ofstream stf(out_path + "_stats.csv");
        stf << "Category,Metric,Value\n"
            << "FRAME,Total_Frames," << s_.total_frames << "\n"
            << "FRAME,Raw_Hit_Frames," << s_.hits_raw << "\n"
            << "FRAME,Headers," << s_.headers << "\n"
            << "FRAME,Trailers," << s_.trailers << "\n"
            << "FRAME,Fillers," << s_.fillers << "\n"
            << "EXCLUDE,WP_Boundary_Cuts," << s_.wp_boundary_cuts << "\n"
            << "EXCLUDE,Events_Missing_Header," << s_.events_missing_header << "\n"
            << "EXCLUDE,Orphan_Data_Words," << s_.orphan_data_words << "\n"
            << "EVENT,Total_Complete_Events," << total_complete << "\n"
            << "EVENT,Events_Perfect_Hits," << s_.events_perfect_hits << "\n"
            << "EVENT,Events_Hit_Mismatch," << s_.events_hit_mismatch << "\n"
            << "EVENT,Events_Discarded_By_Jump," << s_.events_discarded_by_jump << "\n"
            << "EVENT,L1_Counter_Jumps," << s_.l1_counter_jumps << "\n"
            << "EVENT,L1_Jump_Avg," << l1_jump_avg << "\n"
            << "EVENT,L1_Jump_Std," << l1_jump_std << "\n"
            << "EVENT,Buffer_Normal," << s_.l1_buffer_status_counts[0] << "\n"
            << "EVENT,Buffer_HalfFull," << s_.l1_buffer_status_counts[1] << "\n"
            << "EVENT,Buffer_Overflow," << s_.l1_buffer_status_counts[2] << "\n"
            << "EVENT,Buffer_Full," << s_.l1_buffer_status_counts[3] << "\n"
            << "EVENT,CRC_Match," << s_.crc_match << "\n"
            << "EVENT,CRC_Mismatch," << s_.crc_mismatch << "\n"
            << "EVENT,CRC_Err_MissingData," << s_.crc_err_missing << "\n"
            << "EVENT,CRC_Err_SilentFlip," << s_.crc_err_silent << "\n"
            << "HIT,Expected_Total_Hits," << s_.expected_total_hits << "\n"
            << "HIT,Valid_Hits," << s_.valid_hits << "\n"
            << "HIT,Counter_A_Jumps," << s_.counter_a_jumps << "\n"
            << "HIT,Counter_A_Lost_Avg," << cnt_a_jump_avg << "\n"
            << "HIT,Counter_A_Lost_Std," << cnt_a_jump_std << "\n"
            << "HIT,EA_00_Clean," << s_.ea_counts[0] << "\n";
        stf.close();
    }
};

void export_global_stats() {
    TCanvas c_glob("c_glob", "Global", 800, 600);
    gStyle->SetOptStat(111111);
    
    for (auto& kv : g_stats) {
        uint8_t tag = kv.first;
        string label = (tag == 0) ? "RIGHT" : "LEFT";
        string out_path = "results/global_" + label;
        RecoStats& s = kv.second;
        
        ofstream hmf(out_path + "_hitmap.csv");
        for (int r = 0; r < 16; r++) {
            for (int c = 0; c < 16; c++) hmf << s.hit_map[r][c] << (c == 15 ? "" : ",");
            hmf << "\n";
        }
        hmf.close();
        
        if (s.valid_hits > 0) {
            g_toa[tag]->SetFillColor(38); g_toa[tag]->Draw(); c_glob.SaveAs((out_path + "_TOA_CODE.png").c_str());
            g_tot[tag]->SetFillColor(46); g_tot[tag]->Draw(); c_glob.SaveAs((out_path + "_TOT_CODE.png").c_str());
            g_cal[tag]->SetFillColor(30); g_cal[tag]->Draw(); c_glob.SaveAs((out_path + "_CAL_CODE.png").c_str());
            g_toa_ns[tag]->SetFillColor(38); g_toa_ns[tag]->Draw(); c_glob.SaveAs((out_path + "_TOA_ns.png").c_str());
            g_tot_ns[tag]->SetFillColor(46); g_tot_ns[tag]->Draw(); c_glob.SaveAs((out_path + "_TOT_ns.png").c_str());
        }
        if (g_status[tag]->GetEntries() > 0) { g_status[tag]->SetFillColor(41); g_status[tag]->Draw(); c_glob.SaveAs((out_path + "_Status_Code.png").c_str()); }
        if (g_crc[tag]->GetEntries() > 0) { g_crc[tag]->SetFillColor(42); g_crc[tag]->Draw(); c_glob.SaveAs((out_path + "_CRC_Syndrome.png").c_str()); }
        if (g_bcid_delta[tag]->GetEntries() > 0) {
            g_bcid_delta[tag]->SetFillColor(43);
            g_bcid_delta[tag]->GetXaxis()->SetRange(max(1, g_bcid_delta[tag]->FindFirstBinAbove(0) - 5), min(4096, g_bcid_delta[tag]->FindLastBinAbove(0) + 5));
            g_bcid_delta[tag]->Draw(); c_glob.SaveAs((out_path + "_BCID_Delta.png").c_str());
        }
        if (g_hits_per_event[tag]->GetEntries() > 0) {
            g_hits_per_event[tag]->SetFillColor(44);
            g_hits_per_event[tag]->GetXaxis()->SetRange(1, min(257, g_hits_per_event[tag]->FindLastBinAbove(0) + 5));
            g_hits_per_event[tag]->Draw(); c_glob.SaveAs((out_path + "_Hits_Per_Event.png").c_str());
        }
    }
}

void setup_globals(uint8_t tag, const string& label) {
    if (g_toa.find(tag) == g_toa.end()) {
        g_toa[tag] = new TH1D(Form("g_toa_%d", tag), Form("Global TOA_CODE (%s);TOA_CODE;Counts", label.c_str()), 1024, 0, 1024);
        g_tot[tag] = new TH1D(Form("g_tot_%d", tag), Form("Global TOT_CODE (%s);TOT_CODE;Counts", label.c_str()), 512, 0, 512);
        g_cal[tag] = new TH1D(Form("g_cal_%d", tag), Form("Global CAL_CODE (%s);CAL_CODE;Counts", label.c_str()), 1024, 0, 1024);
        g_toa_ns[tag] = new TH1D(Form("g_toa_ns_%d", tag), Form("Global TOA Time (%s);TOA (ns);Counts", label.c_str()), 250, 0, 25);
        g_tot_ns[tag] = new TH1D(Form("g_tot_ns_%d", tag), Form("Global TOT Time (%s);TOT (ns);Counts", label.c_str()), 250, 0, 25);
        g_status[tag] = new TH1D(Form("g_stat_%d", tag), Form("Global Trailer Status Code (%s);Status Code [0-63];Events", label.c_str()), 64, 0, 64);
        g_crc[tag]    = new TH1D(Form("g_crc_%d", tag), Form("Global CRC Syndrome (%s);Syndrome (0=Match);Events", label.c_str()), 256, 0, 256);
        g_bcid_delta[tag] = new TH1D(Form("g_bcid_d_%d", tag), Form("Global Delta BCID (%s);#Delta BCID;Events", label.c_str()), 4096, 0, 4096);
        g_hits_per_event[tag] = new TH1D(Form("g_hpe_%d", tag), Form("Global Hits per Event (%s);Hits;Events", label.c_str()), 257, -0.5, 256.5);
    }
}

int main(int argc, char** argv) {
    int myargc = 1; char* myargv[2] = { argv[0], nullptr };
    TApplication app("app", &myargc, myargv);
    gROOT->SetBatch(kTRUE);
    gErrorIgnoreLevel = kError;

    string folder = "daq_bin_results";
    vector<string> files;
    if (fs::exists(folder)) {
        for (const auto& entry : fs::directory_iterator(folder)) {
            if (entry.path().extension() == ".bin") files.push_back(entry.path().filename().string());
        }
    }
    
    if (files.empty()) { cout << "❌ No .bin files found in daq_bin_results/\n"; return 1; }
    fs::create_directories("results");
    
    setup_globals(0, "RIGHT");
    setup_globals(1, "LEFT");

    for (const string& fn : files) {
        ifstream f(folder + "/" + fn, ios::binary);
        if (!f.is_open()) continue;
        
        string file_prefix = fn.substr(0, fn.find(".bin"));
        cout << "\n>>> 🚀 Analyzing & Plotting: " << fn << " ...\n";
        
        EtrocChannelAnalyzer right_ch("RIGHT", 0x1FFFE, file_prefix, 0);
        EtrocChannelAnalyzer left_ch("LEFT", 0x1FFFE, file_prefix, 1);
        SystemStats sys;
        
        uint32_t magic, size_words;
        uint64_t right_low = 0, left_low = 0;
        bool right_low_ready = false, left_low_ready = false;

        while (f.read((char*)&magic, 4) && f.read((char*)&size_words, 4)) {
            if (magic == MAGIC_LOG || magic == MAGIC_META) {
                f.seekg(size_words * 4, ios::cur);
                continue;
            }
            if (magic == MAGIC_DAT) {
                vector<uint32_t> d(size_words);
                f.read((char*)d.data(), size_words * 4);
                
                for (uint32_t w : d) {
                    if (w == 0) { sys.total_ram_buffer_empty++; continue; }
                    if ((w >> 31) == 0) continue; 
                    
                    uint8_t channel_and_part = (w >> 29) & 0x7; 
                    switch (channel_and_part) {
                        case 0b100: right_low = w & 0xFFFFFF; right_low_ready = true; break;
                        case 0b101: 
                            if (right_low_ready) {
                                right_ch.process_word(((uint64_t)(w & 0xFFFF) << 24) | right_low);
                                right_low_ready = false;
                            } break;
                        case 0b110: left_low = w & 0xFFFFFF; left_low_ready = true; break;
                        case 0b111: 
                            if (left_low_ready) {
                                left_ch.process_word(((uint64_t)(w & 0xFFFF) << 24) | left_low);
                                left_low_ready = false;
                            } break;
                    }
                }
            }
        }
        
        right_ch.finalize_stream(sys);
        left_ch.finalize_stream(sys);
        
        right_ch.export_local_plots_and_data();
        left_ch.export_local_plots_and_data();
    }
    
    cout << "\n>>> 🌍 Exporting Global Summaries...\n";
    export_global_stats();
    cout << "✅ Done! All Local & Global results safely landed in the 'results/' folder.\n";
    return 0;
}