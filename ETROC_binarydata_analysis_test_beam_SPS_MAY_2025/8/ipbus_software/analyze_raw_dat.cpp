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

#include <TApplication.h>
#include <TROOT.h>
#include <TH1D.h>
#include <TCanvas.h>
#include <TStyle.h>
#include <TError.h> 

using namespace std;

// =====================================================================
// 1. Data Structures & ETROC2 Bit Masking 
// =====================================================================
const double T3_NS = 3.125; 

struct RawRecord { 
    uint64_t d; 
    uint8_t chan_tag; 
    uint8_t hw_type; 
};

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
    
    inline uint32_t trl_chipid(uint64_t d) { return (d >> 22) & 0x1FFFF;} 
    inline uint8_t  trl_status(uint64_t d) { return (d >> 16) & 0x3F;   } 
    inline uint16_t trl_hits(uint64_t d)   { return (d >>  8) & 0xFF;   }
    inline uint8_t  trl_crc(uint64_t d)    { return d & 0xFF;           }

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

struct SystemStats {
    uint64_t total_32b_words = 0;
    uint64_t total_ram_buffer_empty = 0;
    void add(const SystemStats& o) {
        total_32b_words += o.total_32b_words;
        total_ram_buffer_empty += o.total_ram_buffer_empty;
    }
};

struct RecoStats {
    uint64_t total_frames = 0, headers = 0, trailers = 0, fillers = 0, hits_raw = 0, valid_hits = 0;
    uint64_t events_missing_header = 0, orphan_data_words = 0, words_dropped_at_start = 0;
    uint64_t wp_boundary_cuts = 0, weird_wp_boundary_cuts = 0;
    uint64_t events_perfect_hits = 0, events_hit_mismatch = 0, events_corrupted_bcid = 0, events_discarded_by_jump = 0;
    uint64_t l1_counter_jumps = 0, l1_jump_sum = 0;
    
    uint64_t ea_counts[4] = {0}, fatal_ea_errors = 0, expected_total_hits = 0, sw_lost_hits = 0;
    uint64_t header_type_counts[4] = {0};
    uint64_t hit_map[16][16] = {0};
    uint64_t fake_trailers_blocked = 0, unknown_frames = 0;

    uint64_t crc_match = 0, crc_mismatch = 0;
    uint64_t crc_err_ea = 0, crc_err_missing = 0, crc_err_missing_with_ea = 0, crc_err_silent = 0;
    
    uint64_t status_counts[64] = {0}; 
    uint64_t l1_buffer_status_counts[4] = {0}; 
    uint64_t seu_error_counts = 0; 
    
    std::map<uint32_t, uint64_t> chip_id_counts;

    void add(const RecoStats& o) {
        total_frames += o.total_frames; headers += o.headers; trailers += o.trailers; 
        fillers += o.fillers; hits_raw += o.hits_raw; valid_hits += o.valid_hits;
        events_missing_header += o.events_missing_header; orphan_data_words += o.orphan_data_words; 
        words_dropped_at_start += o.words_dropped_at_start; wp_boundary_cuts += o.wp_boundary_cuts; 
        weird_wp_boundary_cuts += o.weird_wp_boundary_cuts; events_perfect_hits += o.events_perfect_hits; 
        events_hit_mismatch += o.events_hit_mismatch; events_corrupted_bcid += o.events_corrupted_bcid; 
        events_discarded_by_jump += o.events_discarded_by_jump; l1_counter_jumps += o.l1_counter_jumps; 
        l1_jump_sum += o.l1_jump_sum;
        
        for(int i=0; i<4; i++) {
            ea_counts[i] += o.ea_counts[i];
            header_type_counts[i] += o.header_type_counts[i];
            l1_buffer_status_counts[i] += o.l1_buffer_status_counts[i];
        }
        for(int i=0; i<64; i++) status_counts[i] += o.status_counts[i];
        for(auto& kv : o.chip_id_counts) chip_id_counts[kv.first] += kv.second;

        seu_error_counts += o.seu_error_counts;
        fatal_ea_errors += o.fatal_ea_errors; crc_match += o.crc_match; crc_mismatch += o.crc_mismatch;
        expected_total_hits += o.expected_total_hits; sw_lost_hits += o.sw_lost_hits;
        
        for(int r=0; r<16; r++) for(int c=0; c<16; c++) hit_map[r][c] += o.hit_map[r][c];
        
        fake_trailers_blocked += o.fake_trailers_blocked; unknown_frames += o.unknown_frames;
        crc_err_ea += o.crc_err_ea; crc_err_missing += o.crc_err_missing;
        crc_err_missing_with_ea += o.crc_err_missing_with_ea; crc_err_silent += o.crc_err_silent;
    }
};

// Global 데이터 보관소
std::map<uint8_t, TH1D*> g_toa; 
std::map<uint8_t, TH1D*> g_tot; 
std::map<uint8_t, TH1D*> g_cal;
std::map<uint8_t, TH1D*> g_toa_ns; 
std::map<uint8_t, TH1D*> g_tot_ns; 
std::map<uint8_t, TH1D*> g_status; 
std::map<uint8_t, TH1D*> g_crc;    
std::map<uint8_t, TH1D*> g_bcid_delta; 
std::map<uint8_t, TH1D*> g_hits_per_event; 

std::map<uint8_t, RecoStats> g_stats;
std::map<uint8_t, SystemStats> g_sys;
std::map<uint8_t, uint64_t> g_valid_hits;

// =====================================================================
// 2. Analyzer Class
// =====================================================================
class EtrocChannelAnalyzer {
    string name_, file_prefix_;
    uint32_t expected_chip_id_;
    RecoStats s_;
    uint8_t chan_tag_;
    
    TH1D* h_toa_; TH1D* h_tot_; TH1D* h_cal_;
    TH1D* h_toa_ns_; TH1D* h_tot_ns_;
    TH1D* h_status_; TH1D* h_crc_; TH1D* h_bcid_delta_; TH1D* h_hits_per_event_;
    
    bool is_synced_ = false, expect_l1_jump_ = false, in_event_ = false, has_pending_event_ = false;
    uint64_t pending_trailer_w_ = 0, event_id_ = 0;
    uint32_t hdr_bcid_ = 0, last_bcid_ = 0;
    uint8_t  hdr_type_ = 0, last_l1_cnt_ = 0;
    bool     is_first_event_ = true;
    
    std::vector<uint64_t> current_event_data_;
    std::vector<uint8_t> event_bytes_;
    std::vector<string> hit_stream_buffer_;
    std::vector<uint64_t> recent_words_;
    int cut_print_count_ = 0;
    uint64_t global_word_index_ = 0;

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

        uint8_t buffer_stat = (status >> 4) & 0x3; 
        uint8_t seu_flag    = (status >> 3) & 0x1; 
        s_.l1_buffer_status_counts[buffer_stat]++;
        if (seu_flag) s_.seu_error_counts++;

        h_status_->Fill(status);
        g_status[chan_tag_]->Fill(status);

        uint64_t event_hits = current_event_data_.size();
        h_hits_per_event_->Fill(event_hits);
        g_hits_per_event[chan_tag_]->Fill(event_hits);

        if (trl_h == 0 && event_hits >= 200) trl_h = 256;

        bool current_event_has_ea_error = false;

        for (uint64_t hit_w : current_event_data_) {
            uint8_t ea = etroc::dat_ea(hit_w);
            s_.ea_counts[ea & 0x3]++;
            if (ea >= 0x2) {
                s_.fatal_ea_errors++;
                current_event_has_ea_error = true;
                continue;
            }
            uint8_t col = etroc::dat_col(hit_w);
            uint8_t row = etroc::dat_row(hit_w);
            s_.valid_hits++; s_.hit_map[row][col]++;
            
            uint16_t tot_code = etroc::dat_tot(hit_w);
            uint16_t toa_code = etroc::dat_toa(hit_w);
            uint16_t cal_code = etroc::dat_cal(hit_w);
            double toa_ns = 0.0, tot_ns = 0.0;
            
            if (cal_code > 0) {
                h_toa_->Fill(toa_code);
                g_toa[chan_tag_]->Fill(toa_code);
                h_tot_->Fill(tot_code);
                g_tot[chan_tag_]->Fill(tot_code);
                
                double t_bin = T3_NS / (double)cal_code;
                toa_ns = t_bin * (double)toa_code;
                tot_ns = t_bin * ((2.0 * tot_code) - std::floor(tot_code / 32.0));
                
                h_toa_ns_->Fill(toa_ns);
                g_toa_ns[chan_tag_]->Fill(toa_ns);
                h_tot_ns_->Fill(tot_ns);
                g_tot_ns[chan_tag_]->Fill(tot_ns);
            }
            h_cal_->Fill(cal_code);
            g_cal[chan_tag_]->Fill(cal_code);
            g_valid_hits[chan_tag_]++;
            
            hit_stream_buffer_.push_back(to_string(event_id_) + "," + to_string(row) + "," + to_string(col) + "," + to_string(toa_ns) + "," + to_string(tot_ns) + "," + to_string(cal_code) + "," + to_string(ea));
        }

        push_word_bytes(event_bytes_, pending_trailer_w_);
        uint8_t crc_syndrome = etroc::calculate_crc8(event_bytes_);
        h_crc_->Fill(crc_syndrome);
        g_crc[chan_tag_]->Fill(crc_syndrome);

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
            if (has_missing_hits)           { s_.crc_err_missing++; is_silent = false; }
            if (has_missing_hits && current_event_has_ea_error) s_.crc_err_missing_with_ea++;
            if (is_silent) s_.crc_err_silent++;
        }
        event_id_++;
    }

public:
    EtrocChannelAnalyzer(const string& name, uint32_t chip_id, const string& prefix, uint8_t chan_tag) 
        : name_(name), expected_chip_id_(chip_id), file_prefix_(prefix), chan_tag_(chan_tag) {
        string t_name = name_ + "_" + file_prefix_;
        h_toa_ = new TH1D(Form("h_toa_%s", t_name.c_str()), Form("TOA_CODE Distribution (%s);TOA_CODE [0-1023];Counts", name_.c_str()), 1024, 0, 1024);
        h_tot_ = new TH1D(Form("h_tot_%s", t_name.c_str()), Form("TOT_CODE Distribution (%s);TOT_CODE [0-511];Counts", name_.c_str()), 512, 0, 512);
        h_cal_ = new TH1D(Form("h_cal_%s", t_name.c_str()), Form("CAL_CODE Distribution (%s);CAL_CODE [0-1023];Counts", name_.c_str()), 1024, 0, 1024);
        
        h_toa_ns_ = new TH1D(Form("h_toa_ns_%s", t_name.c_str()), Form("TOA Time Distribution (%s);TOA (ns);Counts", name_.c_str()), 250, 0, 25);
        h_tot_ns_ = new TH1D(Form("h_tot_ns_%s", t_name.c_str()), Form("TOT Time Distribution (%s);TOT (ns);Counts", name_.c_str()), 250, 0, 25);

        h_status_ = new TH1D(Form("h_stat_%s", t_name.c_str()), Form("Trailer Status Code (%s);Status Code [0-63];Events", name_.c_str()), 64, 0, 64);
        h_crc_    = new TH1D(Form("h_crc_%s", t_name.c_str()), Form("CRC Syndrome (%s);CRC Result (0=Match);Events", name_.c_str()), 256, 0, 256);
        h_bcid_delta_ = new TH1D(Form("h_bcid_d_%s", t_name.c_str()), Form("Delta BCID (%s);#Delta BCID (Counts);Events", name_.c_str()), 4096, 0, 4096);
        h_hits_per_event_ = new TH1D(Form("h_hpe_%s", t_name.c_str()), Form("Data Words per Header (%s);Number of Data Words (Hits);Events", name_.c_str()), 257, -0.5, 256.5);
    }
    ~EtrocChannelAnalyzer() { 
        delete h_toa_; delete h_tot_; delete h_cal_; 
        delete h_toa_ns_; delete h_tot_ns_;
        delete h_status_; delete h_crc_; delete h_bcid_delta_; delete h_hits_per_event_; 
    }

    void process_word(uint64_t w, uint64_t file_total_records) {
        global_word_index_++;
        recent_words_.push_back(w);
        if (recent_words_.size() > 10) recent_words_.erase(recent_words_.begin());
        s_.total_frames++;
        
        FrameType payload_t = get_payload_type(w);
        if (payload_t == TRAILER && etroc::trl_chipid(w) != expected_chip_id_) {
            s_.fake_trailers_blocked++;
            payload_t = UNKNOWN;
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
            
            if (in_event_) {
                s_.wp_boundary_cuts++;
                expect_l1_jump_ = true;
                if (global_word_index_ < (file_total_records - 100)) s_.weird_wp_boundary_cuts++;
            }
            
            if (!is_first_event_) {
                if (!expect_l1_jump_) {
                    uint8_t std_cnt = (last_l1_cnt_ + 1) & 0xFF;
                    if (curr_l1_cnt != std_cnt) {
                        s_.l1_counter_jumps++;
                        s_.l1_jump_sum += ((curr_l1_cnt - last_l1_cnt_) & 0xFF);
                        l1_jump_detected = true;
                    }
                }
                int delta_bcid = (curr_bcid - last_bcid_) & 0xFFF;
                h_bcid_delta_->Fill(delta_bcid);
                g_bcid_delta[chan_tag_]->Fill(delta_bcid);
            }
            is_first_event_ = false; last_l1_cnt_ = curr_l1_cnt; last_bcid_ = curr_bcid; expect_l1_jump_ = false;

            if (has_pending_event_) {
                if (l1_jump_detected) s_.events_discarded_by_jump++;
                else commit_pending_event();
                has_pending_event_ = false;
            }
            in_event_ = true;
            current_event_data_.clear(); event_bytes_.clear();
            push_word_bytes(event_bytes_, w);
            hdr_bcid_ = curr_bcid; hdr_type_ = etroc::hdr_type(w);
            return;
        }

        if (payload_t == TRAILER) {
            s_.trailers++;
            if (!in_event_) { s_.events_missing_header++; return; }
            pending_trailer_w_ = w;
            has_pending_event_ = true;
            in_event_ = false;
            return;
        }
        if (payload_t == FILLER) s_.fillers++;
    }

    void finalize_stream(SystemStats& sys) {
        if (has_pending_event_) { commit_pending_event(); has_pending_event_ = false; }
        if (in_event_) { s_.wp_boundary_cuts++; in_event_ = false; }
        g_stats[chan_tag_].add(s_); g_sys[chan_tag_].add(sys);
    }

    void export_local_plots(const string& base_filename) const {
        string prefix = base_filename.substr(0, base_filename.find(".dat"));
        if (prefix.find(".bin") != string::npos) prefix = prefix.substr(0, prefix.find(".bin"));
        size_t last_slash = prefix.find_last_of('/');
        if (last_slash != string::npos) prefix = prefix.substr(last_slash + 1);
        string out_path = "results/" + prefix + "_" + name_;

        TCanvas* c1 = new TCanvas("c1", "Local", 800, 600);
        if (s_.valid_hits > 0) {
            h_toa_->SetFillColor(38); h_toa_->Draw(); c1->SaveAs((out_path + "_TOA_CODE.png").c_str());
            h_tot_->SetFillColor(46); h_tot_->Draw(); c1->SaveAs((out_path + "_TOT_CODE.png").c_str());
            h_cal_->SetFillColor(30); h_cal_->Draw(); c1->SaveAs((out_path + "_CAL_CODE.png").c_str());
            h_toa_ns_->SetFillColor(38); h_toa_ns_->Draw(); c1->SaveAs((out_path + "_TOA_ns.png").c_str());
            h_tot_ns_->SetFillColor(46); h_tot_ns_->Draw(); c1->SaveAs((out_path + "_TOT_ns.png").c_str());
        }
        if (h_status_->GetEntries() > 0) { 
            h_status_->SetFillColor(41); h_status_->Draw(); c1->SaveAs((out_path + "_Status_Code.png").c_str()); 
        }
        if (h_crc_->GetEntries() > 0) { 
            h_crc_->SetFillColor(42); h_crc_->Draw(); c1->SaveAs((out_path + "_CRC_Syndrome.png").c_str()); 
        }
        if (h_bcid_delta_->GetEntries() > 0) { 
            h_bcid_delta_->SetFillColor(43); 
            int first_bin = h_bcid_delta_->FindFirstBinAbove(0);
            int last_bin = h_bcid_delta_->FindLastBinAbove(0);
            h_bcid_delta_->GetXaxis()->SetRange(std::max(1, first_bin - 5), std::min(4096, last_bin + 5));
            h_bcid_delta_->Draw(); c1->SaveAs((out_path + "_BCID_Delta.png").c_str()); 
        }
        if (h_hits_per_event_->GetEntries() > 0) {
            h_hits_per_event_->SetFillColor(44);
            int last_bin = h_hits_per_event_->FindLastBinAbove(0);
            h_hits_per_event_->GetXaxis()->SetRange(1, std::min(257, last_bin + 5));
            h_hits_per_event_->Draw(); c1->SaveAs((out_path + "_Hits_Per_Event.png").c_str());
        }
        delete c1;
    }

    void export_python_data(const string& base_filename, const SystemStats& sys) const {
        string prefix = base_filename.substr(0, base_filename.find(".dat"));
        if (prefix.find(".bin") != string::npos) prefix = prefix.substr(0, prefix.find(".bin"));
        size_t last_slash = prefix.find_last_of('/');
        if (last_slash != string::npos) prefix = prefix.substr(last_slash + 1);
        string out_path_prefix = "results/" + prefix;

        ofstream hsf(out_path_prefix + "_" + name_ + "_hit_stream.csv");
        hsf << "EventID,Row,Col,TOA_ns,TOT_ns,CAL_Code,EA_Code\n";
        for(auto& str : hit_stream_buffer_) hsf << str << "\n";
        hsf.close();

        ofstream hmf(out_path_prefix + "_" + name_ + "_hitmap.csv");
        for (int r = 0; r < 16; r++) {
            for (int c = 0; c < 16; c++) hmf << s_.hit_map[r][c] << (c == 15 ? "" : ",");
            hmf << "\n";
        }
        hmf.close();
    }
};

// =====================================================================
// 3. File Reader & Driver Functions
// =====================================================================
vector<RawRecord> read_raw_dat(const string& path) {
    vector<RawRecord> out; ifstream f(path, ios::binary);
    if (!f.is_open()) return out;
    
    f.seekg(0, ios::end); 
    size_t file_size = f.tellg();
    if (file_size == 0) return out;
    
    size_t n = file_size / 8; 
    f.seekg(0, ios::beg);
    
    out.reserve(n); uint8_t b[8];
    for (size_t i = 0; i < n; i++) {
        f.read((char*)b, 8); if (!f) break;
        uint64_t d = ((uint64_t)b[4]<<32)|((uint64_t)b[3]<<24)|((uint64_t)b[2]<<16)|((uint64_t)b[1]<<8)|(uint64_t)b[0];
        out.push_back({d, b[5], b[6]});
    }
    return out;
}

void run_raw_dat(const string& path) {
    vector<RawRecord> recs = read_raw_dat(path);
    if (recs.empty()) {
        cout << "Processing: " << path << " -> \033[1;33m[SKIPPED] File is empty (0 bytes)\033[0m\n";
        return;
    }
    cout << "Processing: " << path << " -> Loaded " << recs.size() << " records.\n";
    
    map<int,uint64_t> tag_counts;
    for (auto& r : recs) tag_counts[r.chan_tag]++;
    vector<int> chan_tags;
    for (auto& kv : tag_counts) chan_tags.push_back(kv.first);
    size_t last_slash = path.find_last_of('/');
    string prefix = (last_slash == string::npos) ? path : path.substr(last_slash + 1);

    for (int tag : chan_tags) {
        string label = Form("Port_%02x", tag);
        if (tag == 0x0c) label = "Port_L";
        else if (tag == 0x2a) label = "Port_R";
        
        if (g_toa.find(tag) == g_toa.end()) {
            g_toa[tag] = new TH1D(Form("g_toa_%d", tag), Form("Global TOA_CODE Distribution (%s);TOA_CODE [0-1023];Counts", label.c_str()), 1024, 0, 1024);
            g_tot[tag] = new TH1D(Form("g_tot_%d", tag), Form("Global TOT_CODE Distribution (%s);TOT_CODE [0-511];Counts", label.c_str()), 512, 0, 512);
            g_cal[tag] = new TH1D(Form("g_cal_%d", tag), Form("Global CAL_CODE Distribution (%s);CAL_CODE [0-1023];Counts", label.c_str()), 1024, 0, 1024);
            
            g_toa_ns[tag] = new TH1D(Form("g_toa_ns_%d", tag), Form("Global TOA Time Distribution (%s);TOA (ns);Counts", label.c_str()), 250, 0, 25);
            g_tot_ns[tag] = new TH1D(Form("g_tot_ns_%d", tag), Form("Global TOT Time Distribution (%s);TOT (ns);Counts", label.c_str()), 250, 0, 25);

            g_status[tag] = new TH1D(Form("g_stat_%d", tag), Form("Global Trailer Status Code (%s);Status Code [0-63];Events", label.c_str()), 64, 0, 64);
            g_crc[tag]    = new TH1D(Form("g_crc_%d", tag), Form("Global CRC Syndrome (%s);Syndrome (0=Match);Events", label.c_str()), 256, 0, 256);
            g_bcid_delta[tag] = new TH1D(Form("g_bcid_d_%d", tag), Form("Global Delta BCID (%s);#Delta BCID (Counts);Events", label.c_str()), 4096, 0, 4096);
            g_hits_per_event[tag] = new TH1D(Form("g_hpe_%d", tag), Form("Global Data Words per Header (%s);Number of Data Words (Hits);Events", label.c_str()), 257, -0.5, 256.5);
        }
        
        map<uint32_t, uint64_t> chipid_votes;
        for (auto& r : recs) {
            if (r.chan_tag != tag) continue;
            if (r.hw_type == 0x02 && (((r.d >> 9) & 0xFFFF) != 0)) chipid_votes[(r.d >> 22) & 0x1FFFF]++;
        }
        uint32_t detected_chip_id = 0x1ABCD; uint64_t best_votes = 0;
        for (auto& kv : chipid_votes) if (kv.second > best_votes) { best_votes = kv.second; detected_chip_id = kv.first; }
        
        EtrocChannelAnalyzer ana(label, detected_chip_id, prefix, tag);
        uint64_t total_recs = recs.size();
        for (auto& r : recs) {
            if (r.chan_tag != tag) continue;
            if (r.hw_type == 0x02 && ((r.d >> 9) & 0xFFFF) == 0) continue;
            ana.process_word(r.d, total_recs);
        }
        SystemStats sys{};
        ana.finalize_stream(sys);
        ana.export_local_plots(path);
        ana.export_python_data(path, sys);
    }
}

void export_global_stats() {
    TCanvas* c_glob = new TCanvas("c_glob", "Global", 800, 600);
    gStyle->SetOptStat(111111);

    for (auto& kv : g_stats) {
        uint8_t tag = kv.first;
        string label = Form("Port_%02x", tag);
        if (tag == 0x0c) label = "Port_L";
        else if (tag == 0x2a) label = "Port_R";

        string out_path = "results/global_" + label;
        RecoStats& s = kv.second;

        ofstream hmf(out_path + "_hitmap.csv");
        for (int r = 0; r < 16; r++) {
            for (int c = 0; c < 16; c++) hmf << s.hit_map[r][c] << (c == 15 ? "" : ",");
            hmf << "\n";
        }
        hmf.close();

        if (s.valid_hits > 0) {
            g_toa[tag]->SetFillColor(38); g_toa[tag]->Draw(); c_glob->SaveAs((out_path + "_TOA_CODE.png").c_str());
            g_tot[tag]->SetFillColor(46); g_tot[tag]->Draw(); c_glob->SaveAs((out_path + "_TOT_CODE.png").c_str());
            g_cal[tag]->SetFillColor(30); g_cal[tag]->Draw(); c_glob->SaveAs((out_path + "_CAL_CODE.png").c_str());
            
            g_toa_ns[tag]->SetFillColor(38); g_toa_ns[tag]->Draw(); c_glob->SaveAs((out_path + "_TOA_ns.png").c_str());
            g_tot_ns[tag]->SetFillColor(46); g_tot_ns[tag]->Draw(); c_glob->SaveAs((out_path + "_TOT_ns.png").c_str());
        }

        if (g_status[tag]->GetEntries() > 0) {
            g_status[tag]->SetFillColor(41); g_status[tag]->Draw(); c_glob->SaveAs((out_path + "_Status_Code.png").c_str());
        }
        if (g_crc[tag]->GetEntries() > 0) {
            g_crc[tag]->SetFillColor(42); g_crc[tag]->Draw(); c_glob->SaveAs((out_path + "_CRC_Syndrome.png").c_str());
        }
        if (g_bcid_delta[tag]->GetEntries() > 0) {
            g_bcid_delta[tag]->SetFillColor(43); 
            int first_bin = g_bcid_delta[tag]->FindFirstBinAbove(0);
            int last_bin = g_bcid_delta[tag]->FindLastBinAbove(0);
            g_bcid_delta[tag]->GetXaxis()->SetRange(std::max(1, first_bin - 5), std::min(4096, last_bin + 5));
            g_bcid_delta[tag]->Draw(); 
            c_glob->SaveAs((out_path + "_BCID_Delta.png").c_str());
        }
        if (g_hits_per_event[tag]->GetEntries() > 0) {
            g_hits_per_event[tag]->SetFillColor(44);
            int last_bin = g_hits_per_event[tag]->FindLastBinAbove(0);
            g_hits_per_event[tag]->GetXaxis()->SetRange(1, std::min(257, last_bin + 5));
            g_hits_per_event[tag]->Draw();
            c_glob->SaveAs((out_path + "_Hits_Per_Event.png").c_str());
        }

        uint64_t total_complete = s.events_perfect_hits + s.events_hit_mismatch + s.events_corrupted_bcid + s.events_discarded_by_jump;
        uint64_t total_crc_events = s.crc_match + s.crc_mismatch;
        double crc_match_rate = total_crc_events > 0 ? (double)s.crc_match / total_crc_events * 100.0 : 0.0;

        ofstream stf(out_path + "_stats.csv");
        stf << "Category,Metric,Value\n"
            << "FRAME,Total_Frames," << s.total_frames << "\n"
            << "FRAME,Raw_Hit_Frames," << s.hits_raw << "\n"
            << "EVENT,Total_Complete_Events," << total_complete << "\n"
            << "EVENT,CRC_Match," << s.crc_match << "\n"
            << "EVENT,CRC_Mismatch," << s.crc_mismatch << "\n"
            << "EVENT,CRC_Match_Rate_Pct," << crc_match_rate << "\n"
            << "EVENT,Delta_BCID_Mean," << g_bcid_delta[tag]->GetMean() << "\n"
            << "EVENT,Delta_BCID_RMS," << g_bcid_delta[tag]->GetRMS() << "\n"
            << "EVENT,Hits_Per_Event_Mean," << g_hits_per_event[tag]->GetMean() << "\n"
            << "EVENT,Hits_Per_Event_RMS," << g_hits_per_event[tag]->GetRMS() << "\n";
            
        for (int i = 0; i <= 256; i++) {
            uint64_t count = g_hits_per_event[tag]->GetBinContent(i + 1);
            if (count > 0) stf << "EVENT,Hits_Per_Event_Count_" << i << "," << count << "\n";
        }

        stf << "STATUS,L1_Buffer_Normal_00," << s.l1_buffer_status_counts[0] << "\n"
            << "STATUS,L1_Buffer_Half_01," << s.l1_buffer_status_counts[1] << "\n"
            << "STATUS,L1_Buffer_Overflow_10," << s.l1_buffer_status_counts[2] << "\n"
            << "STATUS,L1_Buffer_Full_11," << s.l1_buffer_status_counts[3] << "\n"
            << "STATUS,SEU_Errors_Detected," << s.seu_error_counts << "\n";
            
        for (int i = 0; i < 64; i++) {
            if (s.status_counts[i] > 0) stf << "STATUS,Raw_Status_Code_" << i << "," << s.status_counts[i] << "\n";
        }
        for (auto& kv : s.chip_id_counts) {
            stf << "CHIP_ID,ID_0x" << hex << kv.first << dec << "," << kv.second << "\n";
        }

        stf << "HIT,Expected_Total_Hits," << s.expected_total_hits << "\n"
            << "HIT,Valid_Hits," << s.valid_hits << "\n"
            << "HIT,Lost_Hits," << s.sw_lost_hits << "\n"
            << "HIT,Avg_TOA_ns," << (s.valid_hits > 0 ? g_toa_ns[tag]->GetMean() : 0.0) << "\n"
            << "HIT,RMS_TOA_ns," << (s.valid_hits > 0 ? g_toa_ns[tag]->GetRMS() : 0.0) << "\n"
            << "HIT,Avg_TOT_ns," << (s.valid_hits > 0 ? g_tot_ns[tag]->GetMean() : 0.0) << "\n"
            << "HIT,RMS_TOT_ns," << (s.valid_hits > 0 ? g_tot_ns[tag]->GetRMS() : 0.0) << "\n";
    }
    delete c_glob;
}

int main(int argc, char** argv) {
    int myargc = 1;
    char* myargv[2] = { argv[0], nullptr };
    TApplication app("app", &myargc, myargv);
    
    gROOT->SetBatch(kTRUE);
    gErrorIgnoreLevel = kError; 

    if (argc < 2) {
        cout << "Usage: " << argv[0] << " raw_data/output_run_*_rb0.dat\n";
        return 1;
    }

    cout << "\n========================================================\n";
    cout << "  Batch parsing mode active. Outputs routing to results/\n";
    cout << "========================================================\n";

    for (int i = 1; i < argc; i++) run_raw_dat(argv[i]);

    cout << "\n========================================================\n";
    cout << "  [Diagnostics Report] Data Integrity & Status\n";
    cout << "========================================================\n";
    for (auto& kv : g_stats) {
        uint8_t tag = kv.first;
        string label = Form("Port_%02x", tag);
        if (tag == 0x0c) label = "Port_L";
        else if (tag == 0x2a) label = "Port_R";

        RecoStats& s = kv.second;
        
        uint64_t total_crc = s.crc_match + s.crc_mismatch;
        double crc_rate = total_crc > 0 ? (double)s.crc_match / total_crc * 100.0 : 0.0;

        cout << " [" << label << "]\n";
        cout << "  - CRC Validation : " << fixed << setprecision(2) << crc_rate << "% Match (" 
             << s.crc_match << " matched, " << s.crc_mismatch << " mismatched)\n";
        
        cout << "  - Chip IDs Found : ";
        for (auto& id_kv : s.chip_id_counts) cout << "0x" << hex << id_kv.first << dec << " (" << id_kv.second << " events) ";
        cout << "\n";

        cout << "  - Delta BCID     : Mean = " << fixed << setprecision(2) << g_bcid_delta[tag]->GetMean() 
             << " | RMS = " << g_bcid_delta[tag]->GetRMS() << "\n";

        cout << "  - Hits / Event   : Mean = " << fixed << setprecision(2) << g_hits_per_event[tag]->GetMean() 
             << " | RMS = " << g_hits_per_event[tag]->GetRMS() << "\n";
             
        cout << "    └ Distribution : ";
        int printed_bins = 0;
        for (int i = 0; i <= 256; i++) {
            uint64_t count = g_hits_per_event[tag]->GetBinContent(i + 1);
            if (count > 0) {
                cout << "[" << i << " hits: " << count << "] ";
                printed_bins++;
                if (printed_bins % 4 == 0) cout << "\n                     "; 
            }
        }
        if (printed_bins % 4 != 0) cout << "\n";

        cout << "  - SEU Errors     : " << s.seu_error_counts << " events\n";
        cout << "  - L1 Buffer      : " << s.l1_buffer_status_counts[0] << " Normal | " 
             << s.l1_buffer_status_counts[1] << " Half-Full | "
             << s.l1_buffer_status_counts[2] << " Overflow | "
             << s.l1_buffer_status_counts[3] << " Full\n\n";
    }

    cout << "Exporting Global Summaries & Plots...\n";
    export_global_stats();
    cout << "Done! All results safely landed in the 'results/' folder.\n\n";
    return 0;
}