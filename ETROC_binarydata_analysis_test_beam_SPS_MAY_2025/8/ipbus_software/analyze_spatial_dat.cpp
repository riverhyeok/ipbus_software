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

using namespace std;

// =====================================================================
// 1. ETROC2 Bit Masking & Physical Constants
// =====================================================================
const double T3_NS = 3.125;

namespace etroc {
    static const uint64_t SYNC = 0x3C5C;
    inline bool is_data(uint64_t d)    { return  (d >> 39) & 1; }
    inline bool is_sync(uint64_t d)    { return !is_data(d) && (((d >> 24) & 0x7FFF) == SYNC); }
    inline bool is_header(uint64_t d)  { return  is_sync(d) && (((d >> 22) & 0x3) == 0x0); }
    inline bool is_filler(uint64_t d)  { return  is_sync(d) && (((d >> 22) & 0x3) == 0x2); }
    inline bool is_trailer(uint64_t d) { return !is_data(d) && !is_sync(d); }
    inline uint8_t  dat_ea(uint64_t d)     { return (d >> 37) & 0x3;   }
    inline uint8_t  dat_col(uint64_t d)    { return (d >> 33) & 0xF;   }
    inline uint8_t  dat_row(uint64_t d)    { return (d >> 29) & 0xF;   }
    inline uint16_t dat_toa(uint64_t d)    { return (d >> 19) & 0x3FF; }
    inline uint16_t dat_tot(uint64_t d)    { return (d >> 10) & 0x1FF; }
    inline uint16_t dat_cal(uint64_t d)    { return d & 0x3FF;         }
}

enum FrameType { HEADER = 0, DATA = 1, TRAILER = 2, FILLER = 3, UNKNOWN = 4 };
FrameType get_payload_type(uint64_t w) {
    if (etroc::is_data(w))    return DATA;
    if (etroc::is_header(w))  return HEADER;
    if (etroc::is_filler(w))  return FILLER;
    if (etroc::is_trailer(w)) return TRAILER;
    return UNKNOWN;
}

struct RawRecord { uint64_t d; uint8_t chan_tag; uint8_t hw_type; };

vector<RawRecord> read_raw_dat(const string& path) {
    vector<RawRecord> out;
    ifstream f(path, ios::binary);
    if (!f.is_open()) return out;
    f.seekg(0, ios::end);
    size_t n = (size_t)f.tellg() / 8;
    f.seekg(0, ios::beg);
    out.reserve(n);
    uint8_t b[8];
    for (size_t i = 0; i < n; i++) {
        f.read((char*)b, 8);
        if (!f) break;
        uint64_t d = ((uint64_t)b[4]<<32)|((uint64_t)b[3]<<24)|((uint64_t)b[2]<<16)|((uint64_t)b[1]<<8)|(uint64_t)b[0];
        out.push_back({d, b[5], b[6]});
    }
    return out;
}

// =====================================================================
// 2. Spatial Analysis & Diagnostic Variables
// =====================================================================
double global_com_row = 0; 
double global_com_col = 0;

uint64_t raw_hit_map[256][16][16] = {0}; 
uint64_t cor_hit_map[256][16][16] = {0}; 

// Early Hit 진단용
uint64_t toa_zero_hit_map[256][16][16] = {0}; 

// TOT Second Peak 진단용 (CHA+CHB 통합 맵을 그릴 예정)
uint64_t tot_high_hitmap[256][16][16] = {0}; // TOT >= 4ns
uint64_t tot_low_hitmap[256][16][16] = {0};  // TOT < 4ns

std::map<uint8_t, TH1D*> h_toa_cen;
std::map<uint8_t, TH1D*> h_tot_cen;
std::map<uint8_t, TH1D*> h_cal_cen;

std::map<uint8_t, std::map<int, TH1D*>> h_toa_rad;
std::map<uint8_t, std::map<int, TH1D*>> h_tot_rad;
std::map<uint8_t, std::map<int, TH1D*>> h_cal_rad;

// Raw Trend 데이터 누적
std::map<uint8_t, std::map<int, double>> sum_toa_raw;
std::map<uint8_t, std::map<int, double>> sum_tot_raw;
std::map<uint8_t, std::map<int, double>> sum_cal_raw;
std::map<uint8_t, std::map<int, int>> count_raw;

// Raw Diagnostic Plots (ns 변환 전 순수 Code)
std::map<uint8_t, TH1D*> h_raw_toa;
std::map<uint8_t, TH1D*> h_raw_tot;
std::map<uint8_t, TH1D*> h_raw_cal;

// Early vs Normal Hits
std::map<uint8_t, TH1D*> h_toa_zero_tot;
std::map<uint8_t, TH1D*> h_toa_zero_cal;
std::map<uint8_t, TH1D*> h_normal_tot;
std::map<uint8_t, TH1D*> h_normal_cal;

// TOT 4ns 기준 Second Peak 분석 (CAL, TOA)
std::map<uint8_t, TH1D*> h_tot_high_toa;
std::map<uint8_t, TH1D*> h_tot_high_cal;
std::map<uint8_t, TH1D*> h_tot_low_toa;
std::map<uint8_t, TH1D*> h_tot_low_cal;

// =====================================================================
// 3. Analyzer Class
// =====================================================================
class EtrocChannelAnalyzer {
    uint8_t chan_tag_;
    bool in_event_ = false;
    std::vector<uint64_t> current_event_data_;

    void commit_pending_event() {
        for (uint64_t hit_w : current_event_data_) {
            uint8_t ea = etroc::dat_ea(hit_w);
            if (ea >= 0x2) continue; 
            
            uint8_t col = etroc::dat_col(hit_w);
            uint8_t row = etroc::dat_row(hit_w);
            uint16_t tot_code = etroc::dat_tot(hit_w);
            uint16_t toa_code = etroc::dat_toa(hit_w);
            uint16_t cal_code = etroc::dat_cal(hit_w);

            if (cal_code > 0) {
                double t_bin = T3_NS / (double)cal_code;
                double toa_ns = t_bin * (double)toa_code;
                double tot_ns = t_bin * ((2.0 * tot_code) - std::floor(tot_code / 32.0));

                // Raw Code 저장
                h_raw_toa[chan_tag_]->Fill(toa_code);
                h_raw_tot[chan_tag_]->Fill(tot_code);
                h_raw_cal[chan_tag_]->Fill(cal_code);

                // Center 4 Pixels
                if ((row == 7 || row == 8) && (col == 7 || col == 8)) {
                    h_toa_cen[chan_tag_]->Fill(toa_ns);
                    h_tot_cen[chan_tag_]->Fill(tot_ns);
                    h_cal_cen[chan_tag_]->Fill(cal_code);
                }

                // 방사형 거리 분류
                double dr = (double)row - global_com_row;
                double dc = (double)col - global_com_col;
                int dist = std::round(std::sqrt(dr*dr + dc*dc));

                if (h_toa_rad[chan_tag_].find(dist) == h_toa_rad[chan_tag_].end()) {
                    h_toa_rad[chan_tag_][dist] = new TH1D(Form("toa_rad_%d_%d", chan_tag_, dist), Form("TOA;TOA (ns);Counts"), 250, 0, 25);
                    h_tot_rad[chan_tag_][dist] = new TH1D(Form("tot_rad_%d_%d", chan_tag_, dist), Form("TOT;TOT (ns);Counts"), 250, 0, 25);
                    h_cal_rad[chan_tag_][dist] = new TH1D(Form("cal_rad_%d_%d", chan_tag_, dist), Form("CAL;CAL Code;Counts"), 1024, 0, 1024);
                }
                
                h_toa_rad[chan_tag_][dist]->Fill(toa_ns);
                h_tot_rad[chan_tag_][dist]->Fill(tot_ns);
                h_cal_rad[chan_tag_][dist]->Fill(cal_code);

                // Raw Data 누적
                sum_toa_raw[chan_tag_][dist] += toa_code;
                sum_tot_raw[chan_tag_][dist] += tot_code;
                sum_cal_raw[chan_tag_][dist] += cal_code;
                count_raw[chan_tag_][dist]++;

                // [가설 1] Early Hits 진단
                if (toa_ns < 1.5) {
                    toa_zero_hit_map[chan_tag_][row][col]++;
                    h_toa_zero_tot[chan_tag_]->Fill(tot_ns);
                    h_toa_zero_cal[chan_tag_]->Fill(cal_code);
                } else {
                    h_normal_tot[chan_tag_]->Fill(tot_ns);
                    h_normal_cal[chan_tag_]->Fill(cal_code);
                }

                // [가설 2] TOT Second Peak (4ns 기준 분리)
                if (tot_ns >= 4.0) {
                    h_tot_high_toa[chan_tag_]->Fill(toa_ns);
                    h_tot_high_cal[chan_tag_]->Fill(cal_code);
                    tot_high_hitmap[chan_tag_][row][col]++;
                } else {
                    h_tot_low_toa[chan_tag_]->Fill(toa_ns);
                    h_tot_low_cal[chan_tag_]->Fill(cal_code);
                    tot_low_hitmap[chan_tag_][row][col]++;
                }
            }
        }
    }

public:
    EtrocChannelAnalyzer(uint8_t chan_tag) : chan_tag_(chan_tag) {}

    void process_word(uint64_t w) {
        FrameType payload_t = get_payload_type(w);
        if (payload_t == DATA) {
            if (in_event_) current_event_data_.push_back(w);
            return;
        }
        if (payload_t == HEADER) {
            if (in_event_) commit_pending_event();
            in_event_ = true;
            current_event_data_.clear();
            return;
        }
        if (payload_t == TRAILER && in_event_) {
            commit_pending_event();
            in_event_ = false;
        }
    }
    void finalize() { if (in_event_) commit_pending_event(); }
};

// =====================================================================
// Main Execution
// =====================================================================
int main(int argc, char** argv) {
    int myargc = argc;
    char** myargv = new char*[argc];
    for(int i=0; i<argc; ++i) myargv[i] = argv[i];
    TApplication app("app", &myargc, myargv); 
    
    gROOT->SetBatch(kTRUE);
    gStyle->SetOptStat(0); 
    gStyle->SetPalette(57); 

    system("mkdir -p analyze"); 

    if (argc < 2) {
        cout << "Usage: " << argv[0] << " raw_data/*.dat\n";
        return 1;
    }

    vector<string> files;
    for (int i = 1; i < argc; i++) files.push_back(argv[i]);

    cout << "\n========================================================\n";
    cout << "  [Pass 1] Calculating Center of Mass (Raw & Corrected)... \n";
    cout << "========================================================\n";

    for (const string& file : files) {
        vector<RawRecord> recs = read_raw_dat(file);
        for(auto& r : recs) {
            if (r.hw_type == 0x02 && ((r.d >> 9) & 0xFFFF) == 0) continue; 
            if(etroc::is_data(r.d) && etroc::dat_ea(r.d) < 0x2) {
                raw_hit_map[r.chan_tag][etroc::dat_row(r.d)][etroc::dat_col(r.d)]++;
                cor_hit_map[r.chan_tag][etroc::dat_row(r.d)][etroc::dat_col(r.d)]++;
            }
        }
    }

    double raw_w_sum = 0, raw_r_sum = 0, raw_c_sum = 0;
    for(int r=0; r<16; r++) {
        for(int c=0; c<16; c++) {
            double w = raw_hit_map[0x0c][r][c] + raw_hit_map[0x2a][r][c];
            raw_w_sum += w; raw_r_sum += r * w; raw_c_sum += c * w;
        }
    }
    double raw_com_row = (raw_w_sum > 0) ? raw_r_sum / raw_w_sum : 7.5;
    double raw_com_col = (raw_w_sum > 0) ? raw_c_sum / raw_w_sum : 7.5;

    // Dead Pixel (Row 14, Col 9) 보정
    for (int tag : {0x0c, 0x2a}) {
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
            double w = cor_hit_map[0x0c][r][c] + cor_hit_map[0x2a][r][c];
            cor_w_sum += w; cor_r_sum += r * w; cor_c_sum += c * w;
        }
    }
    global_com_row = (cor_w_sum > 0) ? cor_r_sum / cor_w_sum : 7.5;
    global_com_col = (cor_w_sum > 0) ? cor_c_sum / cor_w_sum : 7.5;

    cout << "  -> RAW Source Center:       (Row: " << fixed << setprecision(4) << raw_com_row << ", Col: " << raw_com_col << ")\n";
    cout << "  -> CORRECTED Source Center: (Row: " << fixed << setprecision(4) << global_com_row << ", Col: " << global_com_col << ")\n";

    for (int tag : {0x0c, 0x2a}) {
        h_toa_cen[tag] = new TH1D(Form("toa_cen_%d", tag), "TOA", 250, 0, 25);
        h_tot_cen[tag] = new TH1D(Form("tot_cen_%d", tag), "TOT", 250, 0, 25);
        h_cal_cen[tag] = new TH1D(Form("cal_cen_%d", tag), "CAL", 1024, 0, 1024);

        // Raw 진단
        h_raw_toa[tag] = new TH1D(Form("raw_toa_%d", tag), "Raw TOA Code", 1024, 0, 1024);
        h_raw_tot[tag] = new TH1D(Form("raw_tot_%d", tag), "Raw TOT Code", 512, 0, 512);
        h_raw_cal[tag] = new TH1D(Form("raw_cal_%d", tag), "Raw CAL Code", 1024, 0, 1024);

        // Early vs Normal 히스토그램
        h_toa_zero_tot[tag] = new TH1D(Form("toa_zero_tot_%d", tag), "TOT for Early Hits (TOA < 1.5ns)", 250, 0, 25);
        h_toa_zero_cal[tag] = new TH1D(Form("toa_zero_cal_%d", tag), "CAL for Early Hits (TOA < 1.5ns)", 1024, 0, 1024);
        h_normal_tot[tag]   = new TH1D(Form("normal_tot_%d", tag), "TOT for Normal Hits (TOA >= 1.5ns)", 250, 0, 25);
        h_normal_cal[tag]   = new TH1D(Form("normal_cal_%d", tag), "CAL for Normal Hits (TOA >= 1.5ns)", 1024, 0, 1024);

        // TOT 피크 분리 (High vs Low)
        h_tot_high_toa[tag] = new TH1D(Form("tot_high_toa_%d", tag), "TOA for TOT >= 4ns", 250, 0, 25);
        h_tot_high_cal[tag] = new TH1D(Form("tot_high_cal_%d", tag), "CAL for TOT >= 4ns", 1024, 0, 1024);
        h_tot_low_toa[tag]  = new TH1D(Form("tot_low_toa_%d", tag), "TOA for TOT < 4ns", 250, 0, 25);
        h_tot_low_cal[tag]  = new TH1D(Form("tot_low_cal_%d", tag), "CAL for TOT < 4ns", 1024, 0, 1024);
    }

    cout << "\n========================================================\n";
    cout << "  [Pass 2] Extracting Spatial TDC Data... \n";
    cout << "========================================================\n";

    EtrocChannelAnalyzer anaA(0x0c);
    EtrocChannelAnalyzer anaB(0x2a);

    for (const string& file : files) {
        vector<RawRecord> recs = read_raw_dat(file);
        for (auto& r : recs) {
            if (r.hw_type == 0x02 && ((r.d >> 9) & 0xFFFF) == 0) continue;
            if (r.chan_tag == 0x0c) anaA.process_word(r.d);
            else if (r.chan_tag == 0x2a) anaB.process_word(r.d);
        }
    }
    anaA.finalize();
    anaB.finalize();

    cout << "\nExporting Plots to 'analyze/' folder...\n";
    TCanvas* c = new TCanvas("c", "Canvas", 1000, 600);

    // ==============================================================
    // [공통 함수] 오버레이(Overlay) 그리기
    // ==============================================================
    auto draw_simple_overlay = [&](TH1D* ha, TH1D* hb, const string& title, const string& filename) {
        c->Clear(); c->SetRightMargin(0.05); c->SetTopMargin(0.10);
        bool hasA = (ha && ha->GetEntries() > 0);
        bool hasB = (hb && hb->GetEntries() > 0);
        if (!hasA && !hasB) return;

        TH1D* href = hasA ? ha : hb;

        double min_x = 999999, max_x = -999999;
        if (hasA) {
            int bin_first = ha->FindFirstBinAbove(0); int bin_last = ha->FindLastBinAbove(0);
            min_x = std::min(min_x, ha->GetXaxis()->GetBinLowEdge(bin_first)); max_x = std::max(max_x, ha->GetXaxis()->GetBinUpEdge(bin_last));
        }
        if (hasB) {
            int bin_first = hb->FindFirstBinAbove(0); int bin_last = hb->FindLastBinAbove(0);
            min_x = std::min(min_x, hb->GetXaxis()->GetBinLowEdge(bin_first)); max_x = std::max(max_x, hb->GetXaxis()->GetBinUpEdge(bin_last));
        }
        if (min_x < max_x) {
            double margin = (max_x - min_x) * 0.1; 
            if (margin == 0) margin = 0.5;
            href->GetXaxis()->SetRangeUser(std::max(0.0, min_x - margin), max_x + margin);
        }

        href->SetTitle(title.c_str());
        double max_val = std::max(hasA ? ha->GetMaximum() : 0, hasB ? hb->GetMaximum() : 0);
        href->SetMaximum(max_val * 1.50);

        if (hasA) { ha->SetLineColor(kBlue); ha->SetLineWidth(2); ha->SetFillColorAlpha(kBlue, 0.3); }
        if (hasB) { hb->SetLineColor(kRed);  hb->SetLineWidth(2); hb->SetFillColorAlpha(kRed, 0.3); }

        if (hasA && hasB) {
            if (ha->GetMaximum() > hb->GetMaximum()) { ha->Draw("HIST"); hb->Draw("HIST SAME"); } 
            else { hb->Draw("HIST"); ha->Draw("HIST SAME"); }
        } else {
            if (hasA) ha->Draw("HIST");
            if (hasB) hb->Draw("HIST");
        }

        TLegend* leg = new TLegend(0.10, 0.82, 0.90, 0.90); leg->SetNColumns(1);
        if (hasA) leg->AddEntry(ha, Form("CH_A (N: %.0f, M: %.2f, RMS: %.2f)", ha->GetEntries(), ha->GetMean(), ha->GetRMS()), "f");
        if (hasB) leg->AddEntry(hb, Form("CH_B (N: %.0f, M: %.2f, RMS: %.2f)", hb->GetEntries(), hb->GetMean(), hb->GetRMS()), "f");
        leg->Draw();
        c->SaveAs(("analyze/" + filename).c_str());
        delete leg;
    };

    // ==============================================================
    // [기능 2] Diag Raw Plots (순수 ADC/TDC Code 분포)
    // ==============================================================
    draw_simple_overlay(h_raw_toa[0x0c], h_raw_toa[0x2a], "Diagnostic: Global Raw TOA Code Distribution;TOA Code;Counts", "Diag_Raw_TOA_Code.png");
    draw_simple_overlay(h_raw_tot[0x0c], h_raw_tot[0x2a], "Diagnostic: Global Raw TOT Code Distribution;TOT Code;Counts", "Diag_Raw_TOT_Code.png");
    draw_simple_overlay(h_raw_cal[0x0c], h_raw_cal[0x2a], "Diagnostic: Global Raw CAL Code Distribution;CAL Code;Counts", "Diag_Raw_CAL_Code.png");

    // ==============================================================
    // [공통 함수] 통합 Hitmap 그리기 (CHA+CHB 통합)
    // ==============================================================
    auto draw_combined_hitmap = [&](uint64_t target_map[256][16][16], const string& title, const string& filename, double com_r, double com_c) {
        c->Clear(); c->SetRightMargin(0.15); c->SetTopMargin(0.10);
        TH2D* h2 = new TH2D("hm", Form("%s;Column (Right=0);Row", title.c_str()), 16, 0, 16, 16, 0, 16);
        for (int r = 0; r < 16; ++r) {
            for (int col = 0; col < 16; ++col) {
                h2->SetBinContent(16 - col, r + 1, target_map[0x0c][r][col] + target_map[0x2a][r][col]);
            }
        }
        for(int i=0; i<16; i++) { h2->GetXaxis()->SetBinLabel(16-i, Form("%d", i)); h2->GetYaxis()->SetBinLabel(i+1, Form("%d", i)); }
        h2->Draw("COLZ");

        double cx = (15.0 - com_c) + 0.5; double cy = com_r + 0.5;
        for(double rad = 0.5; rad < 22; rad += 1.0) {
            double step = 0.05 / rad; bool in_seg = false; TGraph* gr = nullptr; int pt = 0;
            for (double a = 0; a <= 2 * TMath::Pi() + step; a += step) {
                double x = cx + rad * cos(a); double y = cy + rad * sin(a);
                if (x >= 0 && x <= 16 && y >= 0 && y <= 16) {
                    if (!in_seg) { gr = new TGraph(); gr->SetLineColor(kBlack); gr->SetLineStyle(2); gr->SetLineWidth(1); pt = 0; in_seg = true; }
                    gr->SetPoint(pt++, x, y);
                } else {
                    if (in_seg) { if (pt > 1) gr->Draw("L SAME"); in_seg = false; }
                }
            }
            if (in_seg && pt > 1) gr->Draw("L SAME");
        }
        TMarker* marker = new TMarker(cx, cy, 20); marker->SetMarkerColor(kRed); marker->SetMarkerSize(1.5); marker->Draw("SAME");
        TPaveText* pt = new TPaveText(0.40, 0.85, 0.84, 0.95, "NDC");
        pt->SetFillColorAlpha(kWhite, 0.9); pt->SetLineColor(kBlack); pt->SetTextAlign(12); pt->SetTextSize(0.035);
        pt->AddText("Calculated COM:"); pt->AddText(Form("Row %.4f, Col %.4f", com_r, com_c)); pt->Draw("SAME");
        c->SaveAs(("analyze/" + filename).c_str());
        delete h2; delete marker; delete pt;
    };

    // ==============================================================
    // [기존 Hitmaps + Early Hits 진단]
    // ==============================================================
    draw_combined_hitmap(raw_hit_map, "Combined Hitmap (Before Correction)", "Hitmap_Combined_BeforeCorrection.png", raw_com_row, raw_com_col);
    draw_combined_hitmap(cor_hit_map, "Combined Hitmap (After Dead Pixel Correction)", "Hitmap_Combined_AfterCorrection.png", global_com_row, global_com_col);
    draw_combined_hitmap(toa_zero_hit_map, "Diagnostic: Hitmap for Early Hits (TOA < 1.5 ns)", "Hitmap_TOA_Near_Zero.png", global_com_row, global_com_col);

    // Early vs Normal Hit 진단
    draw_simple_overlay(h_toa_zero_tot[0x0c], h_toa_zero_tot[0x2a], "Diagnostic: TOT Distribution for Early Hits (TOA < 1.5 ns);TOT (ns);Counts", "Diag_EarlyHits_TOT_Overlay.png");
    draw_simple_overlay(h_toa_zero_cal[0x0c], h_toa_zero_cal[0x2a], "Diagnostic: CAL Distribution for Early Hits (TOA < 1.5 ns);CAL Code;Counts", "Diag_EarlyHits_CAL_Overlay.png");
    draw_simple_overlay(h_normal_tot[0x0c], h_normal_tot[0x2a], "Diagnostic: TOT Distribution for Normal Hits (TOA >= 1.5 ns);TOT (ns);Counts", "Diag_NormalHits_TOT_Overlay.png");
    draw_simple_overlay(h_normal_cal[0x0c], h_normal_cal[0x2a], "Diagnostic: CAL Distribution for Normal Hits (TOA >= 1.5 ns);CAL Code;Counts", "Diag_NormalHits_CAL_Overlay.png");

    // ==============================================================
    // [기능 3] TOT Second Peak 분석 (4ns 분리: CAL, TOA, HITMAP)
    // ==============================================================
    draw_simple_overlay(h_tot_high_toa[0x0c], h_tot_high_toa[0x2a], "Second Peak Diag: TOA for Hits with TOT >= 4ns;TOA (ns);Counts", "Diag_TOT_High_TOA_Overlay.png");
    draw_simple_overlay(h_tot_high_cal[0x0c], h_tot_high_cal[0x2a], "Second Peak Diag: CAL for Hits with TOT >= 4ns;CAL Code;Counts", "Diag_TOT_High_CAL_Overlay.png");
    draw_combined_hitmap(tot_high_hitmap, "Combined Hitmap (TOT >= 4ns)", "Diag_TOT_High_Hitmap_Combined.png", global_com_row, global_com_col);

    draw_simple_overlay(h_tot_low_toa[0x0c], h_tot_low_toa[0x2a], "Second Peak Diag: TOA for Hits with TOT < 4ns;TOA (ns);Counts", "Diag_TOT_Low_TOA_Overlay.png");
    draw_simple_overlay(h_tot_low_cal[0x0c], h_tot_low_cal[0x2a], "Second Peak Diag: CAL for Hits with TOT < 4ns;CAL Code;Counts", "Diag_TOT_Low_CAL_Overlay.png");
    draw_combined_hitmap(tot_low_hitmap, "Combined Hitmap (TOT < 4ns)", "Diag_TOT_Low_Hitmap_Combined.png", global_com_row, global_com_col);

    // ==============================================================
    // 오버레이 및 미니맵 (거리별 데이터)
    // ==============================================================
    auto draw_overlay_with_minimap = [&](TH1D* ha, TH1D* hb, const string& title, const string& filename, int target_dist) {
        bool hasA = (ha && ha->GetEntries() > 0);
        bool hasB = (hb && hb->GetEntries() > 0);
        if (!hasA && !hasB) return;

        c->Clear();
        TPad* pad_main = new TPad("pad_main", "", 0.0, 0.0, 0.70, 1.0);
        TPad* pad_mini = new TPad("pad_mini", "", 0.70, 0.55, 0.98, 0.95);
        pad_main->SetRightMargin(0.05); pad_main->SetTopMargin(0.10);
        pad_mini->SetMargin(0, 0, 0, 0); 
        pad_main->Draw(); pad_mini->Draw();

        pad_main->cd();
        TH1D* href = hasA ? ha : hb;

        double min_x = 999999, max_x = -999999;
        if (hasA) {
            int bin_first = ha->FindFirstBinAbove(0); int bin_last = ha->FindLastBinAbove(0);
            min_x = std::min(min_x, ha->GetXaxis()->GetBinLowEdge(bin_first)); max_x = std::max(max_x, ha->GetXaxis()->GetBinUpEdge(bin_last));
        }
        if (hasB) {
            int bin_first = hb->FindFirstBinAbove(0); int bin_last = hb->FindLastBinAbove(0);
            min_x = std::min(min_x, hb->GetXaxis()->GetBinLowEdge(bin_first)); max_x = std::max(max_x, hb->GetXaxis()->GetBinUpEdge(bin_last));
        }
        if (min_x < max_x) {
            double margin = (max_x - min_x) * 0.1; 
            if (margin == 0) margin = 0.5;
            href->GetXaxis()->SetRangeUser(std::max(0.0, min_x - margin), max_x + margin);
        }

        href->SetTitle(title.c_str());
        double max_val = std::max(hasA ? ha->GetMaximum() : 0, hasB ? hb->GetMaximum() : 0);
        href->SetMaximum(max_val * 1.50); 

        if (hasA) { ha->SetLineColor(kBlue); ha->SetLineWidth(2); ha->SetFillColorAlpha(kBlue, 0.3); }
        if (hasB) { hb->SetLineColor(kRed);  hb->SetLineWidth(2); hb->SetFillColorAlpha(kRed, 0.3); }

        if (hasA && hasB) {
            if (ha->GetMaximum() > hb->GetMaximum()) { ha->Draw("HIST"); hb->Draw("HIST SAME"); } 
            else { hb->Draw("HIST"); ha->Draw("HIST SAME"); }
        } else {
            if (hasA) ha->Draw("HIST");
            if (hasB) hb->Draw("HIST");
        }

        TLegend* leg = new TLegend(0.10, 0.82, 0.90, 0.90); leg->SetNColumns(1);
        if (hasA) leg->AddEntry(ha, Form("CH_A (N: %.0f, M: %.2f, RMS: %.2f)", ha->GetEntries(), ha->GetMean(), ha->GetRMS()), "f");
        if (hasB) leg->AddEntry(hb, Form("CH_B (N: %.0f, M: %.2f, RMS: %.2f)", hb->GetEntries(), hb->GetMean(), hb->GetRMS()), "f");
        leg->Draw();

        pad_mini->cd(); pad_mini->Range(-3, -3, 19, 19); 
        for(int r=0; r<16; r++) {
            for(int col=0; col<16; col++) {
                TBox* box = new TBox(15-col, r, 16-col, r+1);
                bool active = false;
                if (target_dist == -1) { 
                    if ((r==7||r==8) && (col==7||col==8)) active = true;
                } else { 
                    double dr = r - global_com_row; double dc = col - global_com_col;
                    if (std::round(std::sqrt(dr*dr + dc*dc)) == target_dist) active = true;
                }
                box->SetFillColor(active ? kRed : kWhite);
                box->SetLineColor(kBlack); box->SetLineWidth(1); box->Draw();
            }
        }
        TText* txt1 = new TText(15.5, -1.0, "Col 0"); txt1->SetTextSize(0.08); txt1->SetTextAlign(23); txt1->Draw();
        TText* txt2 = new TText(-0.5, 15.5, "Col 15"); txt2->SetTextSize(0.08); txt2->SetTextAlign(21); txt2->Draw();
        TText* txt3 = new TText(16.5, 0.5, "Row 0"); txt3->SetTextSize(0.08); txt3->SetTextAlign(12); txt3->Draw();
        TText* txt4 = new TText(16.5, 15.5, "Row 15"); txt4->SetTextSize(0.08); txt4->SetTextAlign(12); txt4->Draw();

        c->SaveAs(("analyze/" + filename).c_str());
        delete pad_main; delete pad_mini; delete leg;
    };

    draw_overlay_with_minimap(h_toa_cen[0x0c], h_toa_cen[0x2a], "Center 4 Pixels TOA", "Center_4Pixels_TOA_Overlay.png", -1);
    draw_overlay_with_minimap(h_tot_cen[0x0c], h_tot_cen[0x2a], "Center 4 Pixels TOT", "Center_4Pixels_TOT_Overlay.png", -1);
    draw_overlay_with_minimap(h_cal_cen[0x0c], h_cal_cen[0x2a], "Center 4 Pixels CAL", "Center_4Pixels_CAL_Overlay.png", -1);

    std::set<int> all_dists;
    for(auto& kv : h_toa_rad[0x0c]) if(kv.second->GetEntries() > 0) all_dists.insert(kv.first);
    for(auto& kv : h_toa_rad[0x2a]) if(kv.second->GetEntries() > 0) all_dists.insert(kv.first);

    TGraph* gr_toa_raw_A = new TGraph(); gr_toa_raw_A->SetLineColor(kBlue); gr_toa_raw_A->SetMarkerColor(kBlue); gr_toa_raw_A->SetMarkerStyle(20);
    TGraph* gr_tot_raw_A = new TGraph(); gr_tot_raw_A->SetLineColor(kBlue); gr_tot_raw_A->SetMarkerColor(kBlue); gr_tot_raw_A->SetMarkerStyle(20);
    TGraph* gr_cal_raw_A = new TGraph(); gr_cal_raw_A->SetLineColor(kBlue); gr_cal_raw_A->SetMarkerColor(kBlue); gr_cal_raw_A->SetMarkerStyle(20);
    
    TGraph* gr_toa_raw_B = new TGraph(); gr_toa_raw_B->SetLineColor(kRed); gr_toa_raw_B->SetMarkerColor(kRed); gr_toa_raw_B->SetMarkerStyle(21);
    TGraph* gr_tot_raw_B = new TGraph(); gr_tot_raw_B->SetLineColor(kRed); gr_tot_raw_B->SetMarkerColor(kRed); gr_tot_raw_B->SetMarkerStyle(21);
    TGraph* gr_cal_raw_B = new TGraph(); gr_cal_raw_B->SetLineColor(kRed); gr_cal_raw_B->SetMarkerColor(kRed); gr_cal_raw_B->SetMarkerStyle(21);

    TGraph* gr_toa_ns_A = new TGraph(); gr_toa_ns_A->SetLineColor(kBlue); gr_toa_ns_A->SetMarkerColor(kBlue); gr_toa_ns_A->SetMarkerStyle(20);
    TGraph* gr_tot_ns_A = new TGraph(); gr_tot_ns_A->SetLineColor(kBlue); gr_tot_ns_A->SetMarkerColor(kBlue); gr_tot_ns_A->SetMarkerStyle(20);
    TGraph* gr_toa_ns_B = new TGraph(); gr_toa_ns_B->SetLineColor(kRed); gr_toa_ns_B->SetMarkerColor(kRed); gr_toa_ns_B->SetMarkerStyle(21);
    TGraph* gr_tot_ns_B = new TGraph(); gr_tot_ns_B->SetLineColor(kRed); gr_tot_ns_B->SetMarkerColor(kRed); gr_tot_ns_B->SetMarkerStyle(21);

    int ptA = 0, ptB = 0;
    int ptA_ns = 0, ptB_ns = 0;

    for (int d : all_dists) {
        TH1D* ha_toa = (h_toa_rad[0x0c].count(d)) ? h_toa_rad[0x0c][d] : nullptr;
        TH1D* hb_toa = (h_toa_rad[0x2a].count(d)) ? h_toa_rad[0x2a][d] : nullptr;
        TH1D* ha_tot = (h_tot_rad[0x0c].count(d)) ? h_tot_rad[0x0c][d] : nullptr;
        TH1D* hb_tot = (h_tot_rad[0x2a].count(d)) ? h_tot_rad[0x2a][d] : nullptr;
        TH1D* ha_cal = (h_cal_rad[0x0c].count(d)) ? h_cal_rad[0x0c][d] : nullptr;
        TH1D* hb_cal = (h_cal_rad[0x2a].count(d)) ? h_cal_rad[0x2a][d] : nullptr;

        draw_overlay_with_minimap(ha_toa, hb_toa, Form("Overlay TOA (Distance ~%d)", d), Form("Radial_Dist_%02d_Overlay_TOA.png", d), d);
        draw_overlay_with_minimap(ha_tot, hb_tot, Form("Overlay TOT (Distance ~%d)", d), Form("Radial_Dist_%02d_Overlay_TOT.png", d), d);
        draw_overlay_with_minimap(ha_cal, hb_cal, Form("Overlay CAL (Distance ~%d)", d), Form("Radial_Dist_%02d_Overlay_CAL.png", d), d);

        if (count_raw[0x0c][d] > 50) {
            gr_toa_raw_A->SetPoint(ptA, d, sum_toa_raw[0x0c][d] / count_raw[0x0c][d]);
            gr_tot_raw_A->SetPoint(ptA, d, sum_tot_raw[0x0c][d] / count_raw[0x0c][d]);
            gr_cal_raw_A->SetPoint(ptA, d, sum_cal_raw[0x0c][d] / count_raw[0x0c][d]);
            ptA++;
        }
        if (count_raw[0x2a][d] > 50) {
            gr_toa_raw_B->SetPoint(ptB, d, sum_toa_raw[0x2a][d] / count_raw[0x2a][d]);
            gr_tot_raw_B->SetPoint(ptB, d, sum_tot_raw[0x2a][d] / count_raw[0x2a][d]);
            gr_cal_raw_B->SetPoint(ptB, d, sum_cal_raw[0x2a][d] / count_raw[0x2a][d]);
            ptB++;
        }

        if (ha_toa && ha_toa->GetEntries() > 50) {
            gr_toa_ns_A->SetPoint(ptA_ns, d, ha_toa->GetMean());
            gr_tot_ns_A->SetPoint(ptA_ns, d, ha_tot->GetMean());
            ptA_ns++;
        }
        if (hb_toa && hb_toa->GetEntries() > 50) {
            gr_toa_ns_B->SetPoint(ptB_ns, d, hb_toa->GetMean());
            gr_tot_ns_B->SetPoint(ptB_ns, d, hb_tot->GetMean());
            ptB_ns++;
        }
    }

    // ==============================================================
    // Mean Trend Plot
    // ==============================================================
    auto draw_trend = [&](TGraph* gA, TGraph* gB, const string& title, const string& y_axis, const string& filename) {
        c->Clear();
        c->SetTopMargin(0.10); c->SetRightMargin(0.10);
        if (gA->GetN() == 0 && gB->GetN() == 0) return;
        TMultiGraph* mg = new TMultiGraph();
        if (gA->GetN() > 0) mg->Add(gA, "LP");
        if (gB->GetN() > 0) mg->Add(gB, "LP");
        
        mg->SetTitle(title.c_str());
        mg->Draw("A");
        
        double max_y = -999999, min_y = 999999;
        if (gA->GetN() > 0) {
            max_y = std::max(max_y, TMath::MaxElement(gA->GetN(), gA->GetY()));
            min_y = std::min(min_y, TMath::MinElement(gA->GetN(), gA->GetY()));
        }
        if (gB->GetN() > 0) {
            max_y = std::max(max_y, TMath::MaxElement(gB->GetN(), gB->GetY()));
            min_y = std::min(min_y, TMath::MinElement(gB->GetN(), gB->GetY()));
        }
        if (max_y != -999999 && min_y != 999999) {
            double range = max_y - min_y;
            if (range == 0) range = 0.5;
            mg->GetYaxis()->SetRangeUser(min_y - (range * 0.1), max_y + (range * 0.8)); 
        }

        mg->GetXaxis()->SetTitle("Radial Distance from Global Source Center (pixels)");
        mg->GetYaxis()->SetTitle(y_axis.c_str());
        
        TLegend* leg = new TLegend(0.40, 0.82, 0.88, 0.90);
        if (gA->GetN() > 0) leg->AddEntry(gA, "CH_A Mean", "lp");
        if (gB->GetN() > 0) leg->AddEntry(gB, "CH_B Mean", "lp");
        leg->Draw();
        
        c->SaveAs(("analyze/" + filename).c_str());
        delete leg; delete mg;
    };

    draw_trend(gr_toa_raw_A, gr_toa_raw_B, "Mean TOA Trend by Distance (Raw Data)", "Mean TOA (Raw Code [0-1023])", "Mean_Trend_TOA_Raw.png");
    draw_trend(gr_tot_raw_A, gr_tot_raw_B, "Mean TOT Trend by Distance (Raw Data)", "Mean TOT (Raw Code [0-511])", "Mean_Trend_TOT_Raw.png");
    draw_trend(gr_cal_raw_A, gr_cal_raw_B, "Mean CAL Trend by Distance (Raw Data)", "Mean CAL (Raw Code [0-1023])", "Mean_Trend_CAL_Raw.png");

    draw_trend(gr_toa_ns_A, gr_toa_ns_B, "Mean TOA Trend by Distance (ns)", "Mean TOA (ns)", "Mean_Trend_TOA_ns.png");
    draw_trend(gr_tot_ns_A, gr_tot_ns_B, "Mean TOT Trend by Distance (ns)", "Mean TOT (ns)", "Mean_Trend_TOT_ns.png");

    delete c; delete[] myargv;
    cout << "Done! All diagnostic plots, TOT peak splitting, and trend graphs generated successfully.\n\n";
    return 0;
}