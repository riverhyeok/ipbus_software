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
#include <cstdlib>
#include <set>
#include <filesystem>

// ROOT 라이브러리 포함
#include <TApplication.h>
#include <TROOT.h>
#include <TH1D.h>
#include <TH2D.h>
#include <TEllipse.h>
#include <TMarker.h>
#include <TCanvas.h>
#include <TStyle.h>
#include <TLegend.h>
#include <TGraph.h>
#include <TMultiGraph.h>
#include <TMath.h>
#include <TPaveText.h>
#include <TBox.h>
#include <TText.h>

namespace fs = std::filesystem;
using namespace std;

// =====================================================================
// 1. Data Structures & Physical Constants
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
}

enum FrameType { HEADER = 0, DATA = 1, TRAILER = 2, FILLER = 3, UNKNOWN = 4 };
FrameType get_payload_type(uint64_t w) {
    if (etroc::is_data(w))    return DATA;
    if (etroc::is_header(w))  return HEADER;
    if (etroc::is_filler(w))  return FILLER;
    if (etroc::is_trailer(w)) return TRAILER;
    return UNKNOWN;
}

// 추출된 유효 Hit 구조체 (Pass 2 공간 분석용)
struct HitRecord {
    uint8_t ch; uint8_t row; uint8_t col;
    uint16_t toa; uint16_t tot; uint16_t cal; uint8_t ea;
};

// =====================================================================
// 2. Spatial Analysis Variables & Global Maps
// =====================================================================
double global_com_row = 0;
double global_com_col = 0;
uint64_t raw_hit_map[2][16][16] = {0}; // 0: RIGHT, 1: LEFT
uint64_t cor_hit_map[2][16][16] = {0};
uint64_t toa_zero_hit_map[2][16][16] = {0}; 
uint64_t tot_high_hitmap[2][16][16] = {0}; 
uint64_t tot_low_hitmap[2][16][16] = {0};  

std::map<uint8_t, TH1D*> h_toa_cen, h_tot_cen, h_cal_cen;
std::map<uint8_t, std::map<int, TH1D*>> h_toa_rad, h_tot_rad, h_cal_rad;
std::map<uint8_t, std::map<int, double>> sum_toa_raw, sum_tot_raw, sum_cal_raw;
std::map<uint8_t, std::map<int, int>> count_raw;

std::map<uint8_t, TH1D*> h_raw_toa, h_raw_tot, h_raw_cal;
std::map<uint8_t, TH1D*> h_toa_zero_tot, h_toa_zero_cal, h_normal_tot, h_normal_cal;
std::map<uint8_t, TH1D*> h_tot_high_toa, h_tot_high_cal, h_tot_low_toa, h_tot_low_cal;

std::map<uint8_t, TH1D*> g_toa, g_tot, g_cal, g_toa_ns, g_tot_ns, g_status, g_crc, g_bcid_delta, g_hits_per_event;

// =====================================================================
// 3. Analyzer Class
// =====================================================================
class EtrocChannelAnalyzer {
    string name_;
    uint8_t chan_tag_; // 0: RIGHT, 1: LEFT
    
    bool is_synced_ = false, in_event_ = false, has_pending_event_ = false;
    uint64_t pending_trailer_w_ = 0, last_word_ = 0xFFFFFFFFFFFFFFFFULL;
    
    std::vector<uint64_t> current_event_data_;

public:
    std::vector<HitRecord> all_valid_hits;

    EtrocChannelAnalyzer(const string& name, uint8_t chan_tag) : name_(name), chan_tag_(chan_tag) {}

    void commit_pending_event() {
        for (uint64_t hit_w : current_event_data_) {
            uint8_t ea = etroc::dat_ea(hit_w);
            if (ea >= 0x2) continue; // Fatal Error는 건너뜀
            
            uint8_t col = etroc::dat_col(hit_w);
            uint8_t row = etroc::dat_row(hit_w);
            uint16_t toa = etroc::dat_toa(hit_w);
            uint16_t tot = etroc::dat_tot(hit_w);
            uint16_t cal = etroc::dat_cal(hit_w);
            
            // Pass 1: 원본 히트맵 기록 및 유효 픽셀 저장
            raw_hit_map[chan_tag_][row][col]++;
            all_valid_hits.push_back({chan_tag_, row, col, toa, tot, cal, ea});
        }
    }

    void process_word(uint64_t w) {
        // 🌟 [Stutter 버그 픽스] 중복 단어 방어막
        if (w == last_word_) return;
        last_word_ = w;

        FrameType payload_t = get_payload_type(w);
        if (payload_t == UNKNOWN) return;
        
        if (!is_synced_) {
            if (payload_t == HEADER) is_synced_ = true;
            else return;
        }
        if (payload_t == DATA) {
            if (in_event_) current_event_data_.push_back(w);
            return;
        }
        if (payload_t == HEADER) {
            if (has_pending_event_) commit_pending_event();
            in_event_ = true;
            current_event_data_.clear();
            has_pending_event_ = false;
            return;
        }
        if (payload_t == TRAILER) {
            if (in_event_) {
                pending_trailer_w_ = w;
                has_pending_event_ = true;
                in_event_ = false;
            }
            return;
        }
    }

    void finalize() {
        if (has_pending_event_) commit_pending_event();
    }
};

// =====================================================================
// Main Execution
// =====================================================================
int main(int argc, char** argv) {
    int myargc = 1; char* myargv[2] = { argv[0], nullptr };
    TApplication app("app", &myargc, myargv);
    gROOT->SetBatch(kTRUE);
    gErrorIgnoreLevel = kError;
    gStyle->SetOptStat(0);
    gStyle->SetPalette(57);

    system("mkdir -p results analyze");
    
    string folder = "daq_bin_results";
    vector<string> files;
    if (fs::exists(folder)) {
        for (const auto& entry : fs::directory_iterator(folder)) {
            if (entry.path().extension() == ".bin") files.push_back(entry.path().filename().string());
        }
    }
    if (files.empty()) { cout << "❌ No .bin files found in daq_bin_results/\n"; return 1; }

    cout << "\n========================================================\n";
    cout << "  [Pass 1] Parsing Events & Calculating Center of Mass... \n";
    cout << "========================================================\n";

    EtrocChannelAnalyzer right_ch("RIGHT", 0);
    EtrocChannelAnalyzer left_ch("LEFT", 1);

    for (const string& fn : files) {
        ifstream f(folder + "/" + fn, ios::binary);
        if (!f.is_open()) continue;
        cout << "  -> Loading & Parsing: " << fn << " ...\n";
        
        uint32_t magic, size_words;
        uint64_t right_low = 0, left_low = 0;
        bool right_low_ready = false, left_low_ready = false;

        // IPBus 분리 파싱
        while (f.read((char*)&magic, 4) && f.read((char*)&size_words, 4)) {
            if (magic == MAGIC_LOG || magic == MAGIC_META) {
                f.seekg(size_words * 4, ios::cur); continue;
            }
            if (magic == MAGIC_DAT) {
                vector<uint32_t> d(size_words);
                f.read((char*)d.data(), size_words * 4);
                
                for (uint32_t w : d) {
                    if (w == 0 || (w >> 31) == 0) continue; 
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
    }
    right_ch.finalize();
    left_ch.finalize();

    // COM 계산 로직 (RIGHT + LEFT 통합)
    double raw_w_sum = 0, raw_r_sum = 0, raw_c_sum = 0;
    for(int r=0; r<16; r++) {
        for(int c=0; c<16; c++) {
            cor_hit_map[0][r][c] = raw_hit_map[0][r][c];
            cor_hit_map[1][r][c] = raw_hit_map[1][r][c];
            double w = raw_hit_map[0][r][c] + raw_hit_map[1][r][c];
            raw_w_sum += w; raw_r_sum += r * w; raw_c_sum += c * w;
        }
    }
    double raw_com_row = (raw_w_sum > 0) ? raw_r_sum / raw_w_sum : 7.5;
    double raw_com_col = (raw_w_sum > 0) ? raw_c_sum / raw_w_sum : 7.5;

    // Dead Pixel (Row 14, Col 9) 국소 보정
    for (int tag : {0, 1}) {
        uint64_t sum = 0; int count = 0;
        for(int r=13; r<=15; r++){
            for(int c=8; c<=10; c++){
                if(r>=0 && r<16 && c>=0 && c<16 && !(r==14 && c==9)) {
                    sum += raw_hit_map[tag][r][c]; count++;
                }
            }
        }
        if (count > 0) cor_hit_map[tag][14][9] = sum / count;
    }

    double cor_w_sum = 0, cor_r_sum = 0, cor_c_sum = 0;
    for(int r=0; r<16; r++) {
        for(int c=0; c<16; c++) {
            double w = cor_hit_map[0][r][c] + cor_hit_map[1][r][c];
            cor_w_sum += w; cor_r_sum += r * w; cor_c_sum += c * w;
        }
    }
    global_com_row = (cor_w_sum > 0) ? cor_r_sum / cor_w_sum : 7.5;
    global_com_col = (cor_w_sum > 0) ? cor_c_sum / cor_w_sum : 7.5;

    cout << "\n  -> RAW Source Center:       (Row: " << fixed << setprecision(4) << raw_com_row << ", Col: " << raw_com_col << ")\n";
    cout << "  -> CORRECTED Source Center: (Row: " << fixed << setprecision(4) << global_com_row << ", Col: " << global_com_col << ")\n";

    // 히스토그램 초기화
    for (int tag : {0, 1}) {
        string tname = (tag == 0) ? "RIGHT" : "LEFT";
        h_toa_cen[tag] = new TH1D(Form("toa_cen_%d", tag), "Center 4 Pixels TOA", 250, 0, 25);
        h_tot_cen[tag] = new TH1D(Form("tot_cen_%d", tag), "Center 4 Pixels TOT", 250, 0, 25);
        h_cal_cen[tag] = new TH1D(Form("cal_cen_%d", tag), "Center 4 Pixels CAL", 1024, 0, 1024);
        
        h_raw_toa[tag] = new TH1D(Form("raw_toa_%d", tag), "Raw TOA Code", 1024, 0, 1024);
        h_raw_tot[tag] = new TH1D(Form("raw_tot_%d", tag), "Raw TOT Code", 512, 0, 512);
        h_raw_cal[tag] = new TH1D(Form("raw_cal_%d", tag), "Raw CAL Code", 1024, 0, 1024);
        
        h_toa_zero_tot[tag] = new TH1D(Form("toa_zero_tot_%d", tag), "TOT for Early Hits (TOA < 1.5ns)", 250, 0, 25);
        h_toa_zero_cal[tag] = new TH1D(Form("toa_zero_cal_%d", tag), "CAL for Early Hits (TOA < 1.5ns)", 1024, 0, 1024);
        h_normal_tot[tag]   = new TH1D(Form("normal_tot_%d", tag), "TOT for Normal Hits (TOA >= 1.5ns)", 250, 0, 25);
        h_normal_cal[tag]   = new TH1D(Form("normal_cal_%d", tag), "CAL for Normal Hits (TOA >= 1.5ns)", 1024, 0, 1024);
        
        h_tot_high_toa[tag] = new TH1D(Form("tot_high_toa_%d", tag), "TOA for TOT >= 4ns", 250, 0, 25);
        h_tot_high_cal[tag] = new TH1D(Form("tot_high_cal_%d", tag), "CAL for TOT >= 4ns", 1024, 0, 1024);
        h_tot_low_toa[tag]  = new TH1D(Form("tot_low_toa_%d", tag), "TOA for TOT < 4ns", 250, 0, 25);
        h_tot_low_cal[tag]  = new TH1D(Form("tot_low_cal_%d", tag), "CAL for TOT < 4ns", 1024, 0, 1024);
    }

    cout << "\n========================================================\n";
    cout << "  [Pass 2] Filling Spatial & Diagnostic Distributions... \n";
    cout << "========================================================\n";

    auto fill_hit_data = [&](const std::vector<HitRecord>& hits) {
        for (const auto& h : hits) {
            uint8_t tag = h.ch;
            if (h.cal > 0) {
                double t_bin = T3_NS / (double)h.cal;
                double toa_ns = t_bin * (double)h.toa;
                double tot_ns = t_bin * ((2.0 * h.tot) - std::floor(h.tot / 32.0));

                h_raw_toa[tag]->Fill(h.toa); h_raw_tot[tag]->Fill(h.tot); h_raw_cal[tag]->Fill(h.cal);

                if ((h.row == 7 || h.row == 8) && (h.col == 7 || h.col == 8)) {
                    h_toa_cen[tag]->Fill(toa_ns); h_tot_cen[tag]->Fill(tot_ns); h_cal_cen[tag]->Fill(h.cal);
                }

                double dr = (double)h.row - global_com_row;
                double dc = (double)h.col - global_com_col;
                int dist = std::round(std::sqrt(dr*dr + dc*dc));

                if (h_toa_rad[tag].find(dist) == h_toa_rad[tag].end()) {
                    h_toa_rad[tag][dist] = new TH1D(Form("toa_rad_%d_%d", tag, dist), "TOA (ns)", 250, 0, 25);
                    h_tot_rad[tag][dist] = new TH1D(Form("tot_rad_%d_%d", tag, dist), "TOT (ns)", 250, 0, 25);
                    h_cal_rad[tag][dist] = new TH1D(Form("cal_rad_%d_%d", tag, dist), "CAL Code", 1024, 0, 1024);
                }
                h_toa_rad[tag][dist]->Fill(toa_ns); h_tot_rad[tag][dist]->Fill(tot_ns); h_cal_rad[tag][dist]->Fill(h.cal);

                sum_toa_raw[tag][dist] += h.toa; sum_tot_raw[tag][dist] += h.tot; sum_cal_raw[tag][dist] += h.cal;
                count_raw[tag][dist]++;

                if (toa_ns < 1.5) {
                    toa_zero_hit_map[tag][h.row][h.col]++;
                    h_toa_zero_tot[tag]->Fill(tot_ns); h_toa_zero_cal[tag]->Fill(h.cal);
                } else {
                    h_normal_tot[tag]->Fill(tot_ns); h_normal_cal[tag]->Fill(h.cal);
                }

                if (tot_ns >= 4.0) {
                    h_tot_high_toa[tag]->Fill(toa_ns); h_tot_high_cal[tag]->Fill(h.cal);
                    tot_high_hitmap[tag][h.row][h.col]++;
                } else {
                    h_tot_low_toa[tag]->Fill(toa_ns); h_tot_low_cal[tag]->Fill(h.cal);
                    tot_low_hitmap[tag][h.row][h.col]++;
                }
            }
        }
    };

    fill_hit_data(right_ch.all_valid_hits);
    fill_hit_data(left_ch.all_valid_hits);

    cout << "\nDrawing Overlays and Mini-maps to 'analyze/' folder...\n";
    TCanvas* c = new TCanvas("c", "Canvas", 1000, 600);

    auto draw_simple_overlay = [&](TH1D* ha, TH1D* hb, const string& title, const string& filename) {
        c->Clear(); c->SetRightMargin(0.05); c->SetTopMargin(0.10);
        bool hasA = (ha && ha->GetEntries() > 0); bool hasB = (hb && hb->GetEntries() > 0);
        if (!hasA && !hasB) return;
        TH1D* href = hasA ? ha : hb;
        double min_x = 999999, max_x = -999999;
        if (hasA) { min_x = std::min(min_x, ha->GetXaxis()->GetBinLowEdge(ha->FindFirstBinAbove(0))); max_x = std::max(max_x, ha->GetXaxis()->GetBinUpEdge(ha->FindLastBinAbove(0))); }
        if (hasB) { min_x = std::min(min_x, hb->GetXaxis()->GetBinLowEdge(hb->FindFirstBinAbove(0))); max_x = std::max(max_x, hb->GetXaxis()->GetBinUpEdge(hb->FindLastBinAbove(0))); }
        if (min_x < max_x) { double margin = (max_x - min_x) * 0.1; href->GetXaxis()->SetRangeUser(std::max(0.0, min_x - margin), max_x + margin); }
        href->SetTitle(title.c_str());
        href->SetMaximum(std::max(hasA ? ha->GetMaximum() : 0, hasB ? hb->GetMaximum() : 0) * 1.50);
        if (hasA) { ha->SetLineColor(kBlue); ha->SetLineWidth(2); ha->SetFillColorAlpha(kBlue, 0.3); }
        if (hasB) { hb->SetLineColor(kRed);  hb->SetLineWidth(2); hb->SetFillColorAlpha(kRed, 0.3); }
        if (hasA && hasB) { if (ha->GetMaximum() > hb->GetMaximum()) { ha->Draw("HIST"); hb->Draw("HIST SAME"); } else { hb->Draw("HIST"); ha->Draw("HIST SAME"); } } 
        else { if (hasA) ha->Draw("HIST"); if (hasB) hb->Draw("HIST"); }
        TLegend* leg = new TLegend(0.10, 0.82, 0.90, 0.90);
        if (hasA) leg->AddEntry(ha, Form("RIGHT (N: %.0f, M: %.2f)", ha->GetEntries(), ha->GetMean()), "f");
        if (hasB) leg->AddEntry(hb, Form("LEFT (N: %.0f, M: %.2f)", hb->GetEntries(), hb->GetMean()), "f");
        leg->Draw(); c->SaveAs(("analyze/" + filename).c_str()); delete leg;
    };

    auto draw_combined_hitmap = [&](uint64_t target_map[2][16][16], const string& title, const string& filename, double com_r, double com_c) {
        c->Clear(); c->SetRightMargin(0.15); c->SetTopMargin(0.10);
        TH2D h2("hm", Form("%s;Column (Right=0);Row", title.c_str()), 16, 0, 16, 16, 0, 16);
        for (int r = 0; r < 16; ++r) for (int col = 0; col < 16; ++col) h2.SetBinContent(16 - col, r + 1, target_map[0][r][col] + target_map[1][r][col]);
        for(int i=0; i<16; i++) { h2.GetXaxis()->SetBinLabel(16-i, Form("%d", i)); h2.GetYaxis()->SetBinLabel(i+1, Form("%d", i)); }
        h2.Draw("COLZ");
        double cx = (15.0 - com_c) + 0.5, cy = com_r + 0.5;
        for(double rad = 0.5; rad < 22; rad += 1.0) {
            double step = 0.05 / rad; bool in_seg = false; TGraph* gr = nullptr; int pt = 0;
            for (double a = 0; a <= 2 * TMath::Pi() + step; a += step) {
                double x = cx + rad * cos(a), y = cy + rad * sin(a);
                if (x >= 0 && x <= 16 && y >= 0 && y <= 16) {
                    if (!in_seg) { gr = new TGraph(); gr->SetLineColor(kBlack); gr->SetLineStyle(2); pt = 0; in_seg = true; }
                    gr->SetPoint(pt++, x, y);
                } else { if (in_seg) { if (pt > 1) gr->Draw("L SAME"); in_seg = false; } }
            }
            if (in_seg && pt > 1) gr->Draw("L SAME");
        }
        TMarker marker(cx, cy, 20); marker.SetMarkerColor(kRed); marker.SetMarkerSize(1.5); marker.Draw("SAME");
        TPaveText pt(0.40, 0.85, 0.84, 0.95, "NDC"); pt.SetFillColorAlpha(kWhite, 0.9); pt.SetTextAlign(12); pt.AddText(Form("COM: Row %.4f, Col %.4f", com_r, com_c)); pt.Draw("SAME");
        c->SaveAs(("analyze/" + filename).c_str());
    };

    draw_simple_overlay(h_raw_toa[0], h_raw_toa[1], "Diagnostic: Global Raw TOA Code Distribution", "Diag_Raw_TOA_Code.png");
    draw_simple_overlay(h_raw_tot[0], h_raw_tot[1], "Diagnostic: Global Raw TOT Code Distribution", "Diag_Raw_TOT_Code.png");
    draw_simple_overlay(h_raw_cal[0], h_raw_cal[1], "Diagnostic: Global Raw CAL Code Distribution", "Diag_Raw_CAL_Code.png");

    draw_combined_hitmap(raw_hit_map, "Combined Hitmap (Before Correction)", "Hitmap_Combined_BeforeCorrection.png", raw_com_row, raw_com_col);
    draw_combined_hitmap(cor_hit_map, "Combined Hitmap (After Dead Pixel Correction)", "Hitmap_Combined_AfterCorrection.png", global_com_row, global_com_col);
    draw_combined_hitmap(toa_zero_hit_map, "Diagnostic: Hitmap for Early Hits (TOA < 1.5 ns)", "Hitmap_TOA_Near_Zero.png", global_com_row, global_com_col);

    draw_simple_overlay(h_toa_zero_tot[0], h_toa_zero_tot[1], "Diagnostic: TOT for Early Hits (TOA < 1.5 ns)", "Diag_EarlyHits_TOT_Overlay.png");
    draw_simple_overlay(h_toa_zero_cal[0], h_toa_zero_cal[1], "Diagnostic: CAL for Early Hits (TOA < 1.5 ns)", "Diag_EarlyHits_CAL_Overlay.png");
    draw_simple_overlay(h_normal_tot[0], h_normal_tot[1], "Diagnostic: TOT for Normal Hits (TOA >= 1.5 ns)", "Diag_NormalHits_TOT_Overlay.png");
    draw_simple_overlay(h_normal_cal[0], h_normal_cal[1], "Diagnostic: CAL for Normal Hits (TOA >= 1.5 ns)", "Diag_NormalHits_CAL_Overlay.png");

    draw_simple_overlay(h_tot_high_toa[0], h_tot_high_toa[1], "Second Peak: TOA for Hits with TOT >= 4ns", "Diag_TOT_High_TOA_Overlay.png");
    draw_simple_overlay(h_tot_high_cal[0], h_tot_high_cal[1], "Second Peak: CAL for Hits with TOT >= 4ns", "Diag_TOT_High_CAL_Overlay.png");
    draw_combined_hitmap(tot_high_hitmap, "Combined Hitmap (TOT >= 4ns)", "Diag_TOT_High_Hitmap_Combined.png", global_com_row, global_com_col);

    draw_simple_overlay(h_tot_low_toa[0], h_tot_low_toa[1], "Second Peak: TOA for Hits with TOT < 4ns", "Diag_TOT_Low_TOA_Overlay.png");
    draw_simple_overlay(h_tot_low_cal[0], h_tot_low_cal[1], "Second Peak: CAL for Hits with TOT < 4ns", "Diag_TOT_Low_CAL_Overlay.png");
    draw_combined_hitmap(tot_low_hitmap, "Combined Hitmap (TOT < 4ns)", "Diag_TOT_Low_Hitmap_Combined.png", global_com_row, global_com_col);

    auto draw_overlay_with_minimap = [&](TH1D* ha, TH1D* hb, const string& title, const string& filename, int target_dist) {
        bool hasA = (ha && ha->GetEntries() > 0); bool hasB = (hb && hb->GetEntries() > 0);
        if (!hasA && !hasB) return;
        c->Clear();
        TPad pad_main("pad_main", "", 0.0, 0.0, 0.70, 1.0); TPad pad_mini("pad_mini", "", 0.70, 0.55, 0.98, 0.95);
        pad_main.SetRightMargin(0.05); pad_main.SetTopMargin(0.10); pad_mini.SetMargin(0, 0, 0, 0);
        pad_main.Draw(); pad_mini.Draw(); pad_main.cd();
        TH1D* href = hasA ? ha : hb;
        href->SetTitle(title.c_str()); href->SetMaximum(std::max(hasA ? ha->GetMaximum() : 0, hasB ? hb->GetMaximum() : 0) * 1.50);
        if (hasA) { ha->SetLineColor(kBlue); ha->SetFillColorAlpha(kBlue, 0.3); }
        if (hasB) { hb->SetLineColor(kRed);  hb->SetFillColorAlpha(kRed, 0.3); }
        if (hasA && hasB) { if (ha->GetMaximum() > hb->GetMaximum()) { ha->Draw("HIST"); hb->Draw("HIST SAME"); } else { hb->Draw("HIST"); ha->Draw("HIST SAME"); } } 
        else { if (hasA) ha->Draw("HIST"); if (hasB) hb->Draw("HIST"); }
        
        TLegend leg(0.10, 0.82, 0.90, 0.90);
        if (hasA) leg.AddEntry(ha, Form("RIGHT (M: %.2f, RMS: %.2f)", ha->GetMean(), ha->GetRMS()), "f");
        if (hasB) leg.AddEntry(hb, Form("LEFT (M: %.2f, RMS: %.2f)", hb->GetMean(), hb->GetRMS()), "f");
        leg.Draw();
        
        pad_mini.cd(); pad_mini.Range(-3, -3, 19, 19);
        for(int r=0; r<16; r++) {
            for(int col=0; col<16; col++) {
                TBox box(15-col, r, 16-col, r+1);
                bool active = false;
                if (target_dist == -1) { if ((r==7||r==8) && (col==7||col==8)) active = true; } 
                else { if (std::round(std::sqrt(pow(r-global_com_row, 2) + pow(col-global_com_col, 2))) == target_dist) active = true; }
                box.SetFillColor(active ? kRed : kWhite); box.SetLineColor(kBlack); box.DrawClone();
            }
        }
        TText txt; txt.SetTextSize(0.08); txt.SetTextAlign(23); txt.DrawText(15.5, -1.0, "Col 0"); txt.SetTextAlign(21); txt.DrawText(-0.5, 15.5, "Col 15");
        txt.SetTextAlign(12); txt.DrawText(16.5, 0.5, "Row 0"); txt.DrawText(16.5, 15.5, "Row 15");
        c->SaveAs(("analyze/" + filename).c_str());
    };

    draw_overlay_with_minimap(h_toa_cen[0], h_toa_cen[1], "Center 4 Pixels TOA", "Center_4Pixels_TOA_Overlay.png", -1);
    draw_overlay_with_minimap(h_tot_cen[0], h_tot_cen[1], "Center 4 Pixels TOT", "Center_4Pixels_TOT_Overlay.png", -1);

    std::set<int> all_dists;
    for(auto& kv : h_toa_rad[0]) if(kv.second->GetEntries() > 0) all_dists.insert(kv.first);
    for(auto& kv : h_toa_rad[1]) if(kv.second->GetEntries() > 0) all_dists.insert(kv.first);

    TGraph gr_toa_ns_R, gr_tot_ns_R, gr_toa_ns_L, gr_tot_ns_L;
    gr_toa_ns_R.SetLineColor(kBlue); gr_toa_ns_R.SetMarkerStyle(20); gr_tot_ns_R.SetLineColor(kBlue); gr_tot_ns_R.SetMarkerStyle(20);
    gr_toa_ns_L.SetLineColor(kRed); gr_toa_ns_L.SetMarkerStyle(21); gr_tot_ns_L.SetLineColor(kRed); gr_tot_ns_L.SetMarkerStyle(21);

    int ptR_ns = 0, ptL_ns = 0;
    for (int d : all_dists) {
        TH1D* hr_toa = (h_toa_rad[0].count(d)) ? h_toa_rad[0][d] : nullptr; TH1D* hr_tot = (h_tot_rad[0].count(d)) ? h_tot_rad[0][d] : nullptr;
        TH1D* hl_toa = (h_toa_rad[1].count(d)) ? h_toa_rad[1][d] : nullptr; TH1D* hl_tot = (h_tot_rad[1].count(d)) ? h_tot_rad[1][d] : nullptr;
        
        draw_overlay_with_minimap(hr_toa, hl_toa, Form("Overlay TOA (Distance ~%d)", d), Form("Radial_Dist_%02d_Overlay_TOA.png", d), d);
        draw_overlay_with_minimap(hr_tot, hl_tot, Form("Overlay TOT (Distance ~%d)", d), Form("Radial_Dist_%02d_Overlay_TOT.png", d), d);
        
        if (hr_toa && hr_toa->GetEntries() > 50) { gr_toa_ns_R.SetPoint(ptR_ns, d, hr_toa->GetMean()); gr_tot_ns_R.SetPoint(ptR_ns, d, hr_tot->GetMean()); ptR_ns++; }
        if (hl_toa && hl_toa->GetEntries() > 50) { gr_toa_ns_L.SetPoint(ptL_ns, d, hl_toa->GetMean()); gr_tot_ns_L.SetPoint(ptL_ns, d, hl_tot->GetMean()); ptL_ns++; }
    }

    auto draw_trend = [&](TGraph* gR, TGraph* gL, const string& title, const string& y_axis, const string& filename) {
        c->Clear(); c->SetTopMargin(0.10); c->SetRightMargin(0.10);
        if (gR->GetN() == 0 && gL->GetN() == 0) return;
        TMultiGraph mg;
        if (gR->GetN() > 0) mg.Add(gR, "LP"); if (gL->GetN() > 0) mg.Add(gL, "LP");
        mg.SetTitle(title.c_str()); mg.Draw("A");
        mg.GetXaxis()->SetTitle("Radial Distance from Global Source Center (pixels)"); mg.GetYaxis()->SetTitle(y_axis.c_str());
        TLegend leg(0.40, 0.82, 0.88, 0.90);
        if (gR->GetN() > 0) leg.AddEntry(gR, "RIGHT Mean", "lp"); if (gL->GetN() > 0) leg.AddEntry(gL, "LEFT Mean", "lp");
        leg.Draw(); c->SaveAs(("analyze/" + filename).c_str());
    };

    draw_trend(&gr_toa_ns_R, &gr_toa_ns_L, "Mean TOA Trend by Distance (ns)", "Mean TOA (ns)", "Mean_Trend_TOA_ns.png");
    draw_trend(&gr_tot_ns_R, &gr_tot_ns_L, "Mean TOT Trend by Distance (ns)", "Mean TOT (ns)", "Mean_Trend_TOT_ns.png");

    cout << "\n✅ Done! All advanced spatial diagnostic plots and overlays generated successfully in 'analyze/' folder.\n";
    return 0;
}