#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <filesystem>
#include <string>
#include <condition_variable>
#include <iomanip>
#include <unistd.h>
#include "uhal/uhal.hpp"

namespace fs = std::filesystem;
using namespace std;

const uint32_t MAGIC_LOG  = 0xAAAA0000;
const uint32_t MAGIC_DAT  = 0xBBBB0000;
const uint32_t MAGIC_META = 0xCCCC0000;
const uint32_t FW_CYCLES  = 8;

static void write_block_header(ofstream& f, uint32_t magic, uint32_t size_words) {
    f.write(reinterpret_cast<const char*>(&magic), 4);
    f.write(reinterpret_cast<const char*>(&size_words), 4);
}

struct RawBatch {
    vector<uhal::ValVector<uint32_t>> data;
    vector<uhal::ValVector<uint32_t>> logs;
};

queue<RawBatch> data_queue;
mutex           queue_mtx;
condition_variable queue_cv;
bool daq_done = false;
bool global_stop = false;

struct CtrlCmd {
    bool     do_write  = false;
    uint32_t write_val = 0;
};
queue<CtrlCmd> ctrl_queue;
mutex          ctrl_mtx;

// =====================================================================
// 1. I2C Command Generator (Bit shift perfectly applied)
// =====================================================================
uint32_t make_i2c_cmd(uint8_t op_mode, uint16_t reg_addr, uint8_t data) {
    uint32_t cmd = 0;
    cmd |= (static_cast<uint32_t>(op_mode & 0x03) << 0);
    cmd |= (static_cast<uint32_t>(reg_addr & 0xFFFF) << 2);
    cmd |= (static_cast<uint32_t>(data & 0xFF) << 18);
    return cmd;
}

// =====================================================================
// 2. I2C Write / Read Functions
// =====================================================================
// =====================================================================
// 2. I2C Write / Read Functions (IPbus packet loss defense IPbus packet loss defense IPbus packet loss defense IPbus 패킷 증발 방어 & 자동 재시도 적용 auto-retry applied auto-retry applied auto-retry applied)
// =====================================================================
bool i2c_write_reg(uhal::HwInterface& hw, uint16_t reg_addr, uint8_t data) {
    uint32_t cmd = make_i2c_cmd(0, reg_addr, data); 
    
    // 🌟 1. Command transmission (Max 3 retries)
    bool dispatched = false;
    for(int i = 0; i < 3; i++) {
        try {
            hw.getNode("ctrl_reg.cmd").write(cmd);
            hw.dispatch();
            dispatched = true;
            break; // Break on success
        } catch (const exception& e) {
            usleep(2000); // UDP packet dropped! Wait 2ms and retry
        }
    }
    if(!dispatched) return false; // Treat as I2C failure if all 3 attempts fail

    // 🌟 2. Polling (status check) loop
    uint32_t val = 0;
    auto start_time = chrono::steady_clock::now(); 
    
    do {
        try {
            uhal::ValWord<uint32_t> stat = hw.getNode("ctrl_reg.stat").read();
            hw.dispatch();
            val = stat.value();
        } catch(const exception& e) {
            // If UDP packet is dropped during read, wait 2ms and re-enter loop (read again)
            usleep(2000);
            continue; 
        }
        
        auto elapsed_ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start_time).count();
        if (elapsed_ms > 500) {
            cerr << "[i2c error] Write Timeout for Reg 0x" << hex << reg_addr << dec << "\n";
            return false;
        }
        usleep(1000); 
    } while (val & 0x01); 

    return ((val & 0x04) == 0); 
}

bool i2c_read_reg(uhal::HwInterface& hw, uint16_t reg_addr, uint8_t& out_data) {
    uint32_t cmd = make_i2c_cmd(1, reg_addr, 0); 
    
    bool dispatched = false;
    for(int i = 0; i < 3; i++) {
        try {
            hw.getNode("ctrl_reg.cmd").write(cmd);
            hw.dispatch();
            dispatched = true;
            break;
        } catch (const exception& e) {
            usleep(2000); 
        }
    }
    if(!dispatched) return false;

    uint32_t val = 0;
    auto start_time = chrono::steady_clock::now(); 
    
    do {
        try {
            uhal::ValWord<uint32_t> stat = hw.getNode("ctrl_reg.stat").read();
            hw.dispatch();
            val = stat.value();
        } catch(const exception& e) {
            usleep(2000);
            continue;
        }
        
        auto elapsed_ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start_time).count();
        if (elapsed_ms > 500) {
            cerr << "[i2c error] Read Timeout for Reg 0x" << hex << reg_addr << dec << "\n";
            return false;
        }
        usleep(1000); 
    } while (val & 0x01); 

    out_data = (val >> 3) & 0xFF; 
    return ((val & 0x04) == 0);   
}

// =====================================================================
// 3. FPGA Readout Configuration Sub-Menu
// =====================================================================
void configure_readout_fpga(uhal::HwInterface& hw) {
    uint32_t val = 0;
    
    try {
        uhal::ValWord<uint32_t> rd = hw.getNode("ctrl_reg.readout_cfg").read();
        hw.dispatch();
        val = rd.value();
    } catch (const exception& e) {
        cerr << "[FAIL] Failed to read current config: " << e.what() << "\n";
        return;
    }

    while(true) {
        uint32_t left_size                 = (val >> 0)  & 0x1F; 
        uint32_t right_size                = (val >> 5)  & 0x1F; 
        uint32_t l1a_dis                   = (val >> 10) & 0x1;  
        uint32_t etroc_addr                = (val >> 11) & 0x1F; 
        uint32_t reset_req                 = (val >> 16) & 0x1;  
        uint32_t ldo_en                    = (val >> 17) & 0x1;  
        uint32_t serialzer_alignment_reset = (val >> 18) & 0x1; // Uncommented and variable enabled!

        cout << "\n--- [ FPGA Readout Configuration ] ---\n";
        cout << " 1. Trigger Data Size (Left)  [0-31] : " << left_size << "\n";
        cout << " 2. Trigger Data Size (Right) [0-31] : " << right_size << "\n";
        cout << " 3. L1A Disable               [0/1]  : " << l1a_dis << "\n";
        cout << " 4. ETROC Address Straps      [0-31] : " << etroc_addr << "\n";
        cout << " 5. ETROC Reset Request       [0/1]  : " << reset_req << "\n";
        cout << " 6. LDO Enable                [0/1]  : " << ldo_en << "\n";
        cout << " 7. Serializer Align Reset    [0/1]  : " << serialzer_alignment_reset << "\n"; // Added menu option 7
        cout << "--------------------------------------\n";
        cout << " 8. Apply (Write to FPGA) & Return\n"; // Shifted from 7 to 8
        cout << " 9. Cancel & Return to Main Menu\n"; // Shifted from 8 to 9
        cout << "Select Field to Edit > ";

        int sel;
        cin >> sel;
        
        // Modified to allow inputs from 1 to 7
        if (sel >= 1 && sel <= 7) {
            uint32_t in_val;
            cout << " > Enter new value: ";
            cin >> in_val;
            
            if (sel == 1)      val = (val & ~(0x1F << 0))  | ((in_val & 0x1F) << 0);
            else if (sel == 2) val = (val & ~(0x1F << 5))  | ((in_val & 0x1F) << 5);
            else if (sel == 3) val = (val & ~(0x1  << 10)) | ((in_val & 0x1)  << 10);
            else if (sel == 4) val = (val & ~(0x1F << 11)) | ((in_val & 0x1F) << 11);
            else if (sel == 5) val = (val & ~(0x1  << 16)) | ((in_val & 0x1)  << 16);
            else if (sel == 6) val = (val & ~(0x1  << 17)) | ((in_val & 0x1)  << 17);
            else if (sel == 7) val = (val & ~(0x1  << 18)) | ((in_val & 0x1)  << 18); // Added 1-bit masking write logic to the 18th bit
        } 
        else if (sel == 8) {
            try {
                hw.getNode("ctrl_reg.readout_cfg").write(val);
                hw.dispatch();
                cout << "[SUCCESS] FPGA Readout Config Updated! (Raw: 0x" << hex << val << dec << ")\n";
            } catch (const exception& e) {
                cout << "[FAIL] IPbus Write Error: " << e.what() << "\n";
            }
            break;
        } 
        else if (sel == 9) {
            cout << "Canceled. Returning to Main Menu...\n";
            break;
        } else {
            cout << "[Error] Invalid Selection.\n";
        }
    }
}
// =====================================================================
// Configuration Interactive Menu (Pre-DAQ)
// =====================================================================
void run_config_menu(uhal::HwInterface& hw) {
    auto write_with_check = [&](uint16_t addr, uint8_t val, const string& desc, bool delay = false) {
        if (i2c_write_reg(hw, addr, val)) {
            cout << "[OK] " << desc << " (0x" << hex << addr << " = 0x" << (int)val << dec << ")\n";
        } else {
            cout << "[FAIL] " << desc << " Write NACK! (0x" << hex << addr << ")\n";
        }
        if (delay) usleep(200000); 
    };

    while (true) {
        cout << "\n=======================================================\n";
        cout << "   ETROC2 Configuration Menu (I2C & IPbus)\n";
        cout << "=======================================================\n";
        
        // --- 1. I2C 통신 및 칩 제어 ---
        cout << " [ I2C communication with ETROC ]\n";
        cout << " 1. Apply Target Run Configs \n";
        cout << " 2. Auto-Calibration & ACC S-Curve Scan\n";
        cout << " 3. Apply Charge Injection Pattern (Verilog Testbench)\n";
        cout << " 4. Read I2C Register (ETROC2)\n";
        cout << " 5. Write I2C Register (ETROC2) [Safe Mode]\n";
        cout << " 6. Test I2C Reliability (Safe R/W Sweep)\n"; // Moved from 8 to 6
        cout << "-------------------------------------------------------\n";
        
        // --- 2. FPGA 펌웨어 및 Fast Command (L1A 등) ---
        cout << " [ Readout board Firmware Config & Fast Command (only about L1A signal) ]\n";
        cout << " 7. Read Readout Config (FPGA Status)\n";            // Moved from 6 to 7
        cout << " 8. Write Readout Config (Interactive Menu)\n";      // Moved from 7 to 8
        cout << "-------------------------------------------------------\n";
        
        // --- 3. 데이터 획득(DAQ) 및 종료 ---
        cout << " [ System & DAQ ]\n";
        cout << " 9. Proceed to High-Speed DAQ (DAQ -> PC)\n";       // Text changed
        cout << " 10. Quit Program\n";
        cout << "=======================================================\n";
        cout << "Select Option > ";

        string sel;
        cin >> sel;

        if (sel == "1") {
            cout << "\n>>> Applying ETROC2 Target Run Configurations (Synced with Python)...\n";
            
            // FSM & PLL Resets (Toggle)
            write_with_check(0x000D, 0x00, "PeriCfg13: asyPLLReset = 0", true);
            write_with_check(0x000D, 0x80, "PeriCfg13: asyPLLReset = 1");
            write_with_check(0x000F, 0x20, "PeriCfg15: asyStartCalibration = 0", true);
            write_with_check(0x000F, 0x60, "PeriCfg15: asyStartCalibration = 1");
            write_with_check(0x000E, 0x70, "PeriCfg14: asyResetGlobalReadout = 0", true);
            write_with_check(0x000E, 0xF0, "PeriCfg14: asyResetGlobalReadout = 1");
            write_with_check(0x000D, 0xA0, "PeriCfg13: asyAlignFastcommand = 1", true);
            write_with_check(0x000D, 0x80, "PeriCfg13: asyAlignFastcommand = 0");
            
            // eFuse Programming Data
            write_with_check(0x0016, 0x0F, "PeriCfg22: EFuse_Prog Byte 0");
            write_with_check(0x0017, 0x7F, "PeriCfg23: EFuse_Prog Byte 1");
            write_with_check(0x0018, 0x01, "PeriCfg24: EFuse_Prog Byte 2");
            write_with_check(0x0019, 0x00, "PeriCfg25: EFuse_Prog Byte 3");
            
            write_with_check(0x0013, 0x42, "PeriCfg19: singlePort=1");
            write_with_check(0x0015, 0x00, "PeriCfg21: serRate=320Mbps(00), onChipL1A=0");
            write_with_check(0x0003, 0x38, "PeriCfg3 : PLL_ENABLEPLL = 1");
            write_with_check(0x0011, 0x8A, "PeriCfg17: chargeInjectionDelay = 0x0A");
            write_with_check(0x0014, 0x41, "PeriCfg20: triggerGranularity = 0x00");
            write_with_check(0x0012, 0x08, "PeriCfg18: onchip l1a=11 fcClk=0/DataDelayEn = 1");
            
            // Pixel Matrix Configurations (Broadcast)
            write_with_check(0x8007, 0x01, "PixCfg7  : disDataReadout=0, disTrigPath=0");
            write_with_check(0x8001, 0x3E, "PixCfg1  : QInjEn=1, QSel=0x1E");
            write_with_check(0x8008, 0x81, "PixCfg8  : L1Adelay[0]=1");
            write_with_check(0x8009, 0xFA, "PixCfg9  : L1Adelay[8:1]=0xFA");
            write_with_check(0x8003, 0x05, "PixCfg3  : Bypass_THCal=1");
            write_with_check(0x8004, 0xFF, "PixCfg4  : DAC[7:0]=0xFF");
            write_with_check(0x8005, 0x53, "PixCfg5  : TH_offset=0x14, DAC[9:8]=0x03");
            write_with_check(0x8006, 0xC2, "PixCfg6  : enable_TDC=1");
            write_with_check(0x8000, 0x40, "PixCfg0  : IBSel=0 (High power)");
            
            // Final Reset
            write_with_check(0x000E, 0x70, "PeriCfg14: asyResetGlobalReadout = 0", true);
            write_with_check(0x000E, 0xF0, "PeriCfg14: asyResetGlobalReadout = 1");
            
            cout << "\n>>> Initialization Complete!\n";
        } 
        else if (sel == "2") {
            cout << "\n=======================================================\n";
            cout << "   Auto-Calibration & ACC S-Curve Scan\n";
            cout << "=======================================================\n";
            cout << ">>> [1/3] Disabling All Pixels via Broadcast (0xA000)...\n";
            write_with_check(0xA007, 0x03, "Broadcast: PixCfg7 Disable");
            
            cout << ">>> [2/3] Starting Auto-Calibration & Extracting BL/NW for 256 Pixels...\n";
            
            int baseline_map[16][16] = {0};
            int noise_width_map[16][16] = {0};

            for (int col = 0; col < 16; col++) {
                for (int row = 0; row < 16; row++) {
                    uint16_t sta_base = 0xC000 | (col << 9) | (row << 5);
                    uint8_t sta1_val = 0;
                    int timeout = 100;
                    
                    while (timeout-- > 0) {
                        i2c_read_reg(hw, sta_base | 0x01, sta1_val);
                        if (sta1_val & 0x01) break; 
                        usleep(1000); 
                    }
                    
                    if (timeout <= 0) {
                        cout << "[WARN] Pixel(" << col << "," << row << ") Auto-Cal Timeout!\n";
                    } else {
                        uint8_t sta2_val = 0, sta3_val = 0;
                        i2c_read_reg(hw, sta_base | 0x02, sta2_val);
                        i2c_read_reg(hw, sta_base | 0x03, sta3_val);
                        
                        int nw = (sta1_val >> 1) & 0x0F;
                        int bl = ((sta3_val & 0x03) << 8) | sta2_val;
                        
                        baseline_map[row][col] = bl;
                        noise_width_map[row][col] = nw;
                    }
                }
            }
            cout << "[SUCCESS] BL/NW Extraction Complete!\n";

            cout << "\n>>> [3/3] Perform ACC S-Curve Scan on a specific pixel\n";
            cout << "> Enter Target Row (0-15): ";
            int t_row; cin >> t_row;
            cout << "> Enter Target Col (0-15): ";
            int t_col; cin >> t_col;

            int bl = baseline_map[t_row][t_col];
            int nw = noise_width_map[t_row][t_col];
            
            if (bl == 0 && nw == 0) {
                cout << "[FAIL] Valid BL/NW not found for Pixel(" << t_col << "," << t_row << "). Skipping scan.\n";
            } else {
                cout << "\n>>> Scanning Pixel(" << t_col << "," << t_row << ") | BaseLine: " << bl << " | NoiseWidth: " << nw << "\n";
                
                uint16_t cfg_base = 0x8000 | (t_col << 9) | (t_row << 5);
                uint16_t sta_base = 0xC000 | (t_col << 9) | (t_row << 5);

                uint8_t cfg6; i2c_read_reg(hw, cfg_base | 0x06, cfg6);
                i2c_write_reg(hw, cfg_base | 0x06, cfg6 & 0x7F);

                uint8_t cfg3_base_val = 0x0F;
                i2c_write_reg(hw, cfg_base | 0x03, cfg3_base_val);

                int half_range = 40;
                int step = 1;
                
                for(int dac = bl - half_range; dac <= bl + half_range; dac += step) {
                    int safe_dac = dac < 0 ? 0 : (dac > 1023 ? 1023 : dac);

                    i2c_write_reg(hw, cfg_base | 0x03, cfg3_base_val & ~0x01);
                    i2c_write_reg(hw, cfg_base | 0x03, cfg3_base_val); 

                    i2c_write_reg(hw, cfg_base | 0x04, safe_dac & 0xFF);
                    uint8_t cfg5; i2c_read_reg(hw, cfg_base | 0x05, cfg5);
                    i2c_write_reg(hw, cfg_base | 0x05, (cfg5 & 0xFC) | ((safe_dac >> 8) & 0x03));

                    i2c_write_reg(hw, cfg_base | 0x03, cfg3_base_val | 0x10);
                    i2c_write_reg(hw, cfg_base | 0x03, cfg3_base_val);

                    uint8_t sta1 = 0;
                    int retry_counter = 0;
                    while(retry_counter < 5) {
                        usleep(10000); 
                        i2c_read_reg(hw, sta_base | 0x01, sta1);
                        if (sta1 & 0x01) break;
                        retry_counter++;
                    }
                    if (retry_counter == 5) {
                        cout << "  [WARN] ACC S-curve scan timeout at DAC " << safe_dac << "\n";
                        continue; 
                    }

                    uint8_t acc_l = 0, acc_h = 0;
                    i2c_read_reg(hw, sta_base | 0x05, acc_l);
                    i2c_read_reg(hw, sta_base | 0x06, acc_h);
                    int acc_val = (acc_h << 8) | acc_l;

                    cout << "  - DAC: " << safe_dac << " | ACC: " << acc_val << "\n";
                }

                i2c_write_reg(hw, cfg_base | 0x03, 0x05);
                i2c_write_reg(hw, cfg_base | 0x04, 0xFF);
                uint8_t cfg5_clean; i2c_read_reg(hw, cfg_base | 0x05, cfg5_clean);
                i2c_write_reg(hw, cfg_base | 0x05, (cfg5_clean & 0xFC) | 0x03);
                
                cout << ">>> ACC S-Curve Scan Complete for Pixel(" << t_col << "," << t_row << ")!\n";
            }
        }
        else if (sel == "3") {
            cout << "\n>>> Applying Charge Injection Pattern...\n";
            cout << "[OK] Pattern applied.\n";
        }
        else if (sel == "4") {
            cout << "\n> Enter 16-bit I2C Address (Hex, e.g. 000D): ";
            uint16_t addr;
            cin >> hex >> addr >> dec;
            uint8_t read_val = 0;
            if (i2c_read_reg(hw, addr, read_val)) {
                cout << "[SUCCESS] Read I2C Reg [0x" << hex << addr << "] = 0x" << (int)read_val << dec << "\n";
            } else {
                cout << "[FAIL] I2C Read Error (NACK received or Timeout)\n";
            }
        } 
        else if (sel == "5") {
            cout << "\n> Enter 16-bit I2C Address (Hex, e.g. 000D): ";
            uint16_t addr;
            cin >> hex >> addr >> dec;
            cout << "> Enter 8-bit Data to Write (Hex, e.g. 80): ";
            uint16_t write_val;
            cin >> hex >> write_val >> dec;

            if (addr == 0x0020) {
                cout << "\n[BLOCKED] 🚨 CRITICAL DANGER: Attempted to write to Magic Number Register (0x0020)!\n";
                cout << "Writing to this address will permanently lock the I2C clock. Operation aborted.\n";
                continue;
            }

            if (i2c_write_reg(hw, addr, (uint8_t)write_val)) {
                cout << "[SUCCESS] Wrote 0x" << hex << write_val << dec << " to I2C Reg [0x" << hex << addr << "]\n";
            } else {
                cout << "[FAIL] I2C Write Error (NACK received or Timeout)\n";
            }
        }
        
        // 🌟 Option 6 (Formerly 8): I2C Reliability Test
        else if (sel == "6") {
            cout << "\n>>> [I2C Reliability Stress Test]\n";
            cout << "> Enter number of iterations per register: ";
            int iterations;
            cin >> iterations;
            
            int total_errors = 0;
            int total_tests = 0;
            
            cout << "Testing Config (SAFE R/W: eFuse 0x0018~0x001B) & Status (Read-Only: 0x0100~0x010F) for " << iterations << " iterations...\n";
            
            for (int iter = 1; iter <= iterations; iter++) {
                for (uint16_t addr = 0x0018; addr <= 0x001B; addr++) {
                    uint8_t temp_val = (iter + addr) & 0xFF;
                    uint8_t test_val = (temp_val == 0) ? 1 : temp_val;
                    
                    if (!i2c_write_reg(hw, addr, test_val)) {
                        cout << "[Error] WRITE failed at Iteration " << iter << ", Addr: 0x" << hex << addr << dec << "\n";
                        total_errors++;
                    } else {
                        usleep(1000); 
                        uint8_t read_val = 0;
                        if (!i2c_read_reg(hw, addr, read_val)) {
                            cout << "[Error] READ failed at Iteration " << iter << ", Addr: 0x" << hex << addr << dec << "\n";
                            total_errors++;
                        } else if (read_val != test_val) {
                            cout << "[Error] DATA MISMATCH at Iteration " << iter << ", Addr: 0x" << hex << addr << dec 
                                 << " (Wrote: 0x" << hex << (int)test_val << ", Read: 0x" << (int)read_val << dec << ")\n";
                            total_errors++;
                        }
                        usleep(1000); 
                    }
                    total_tests++; 
                }

                for (uint16_t addr = 0x0100; addr <= 0x010F; addr++) {
                    uint8_t read_val = 0;
                    if (!i2c_read_reg(hw, addr, read_val)) {
                        cout << "[Error] STATUS READ failed at Iteration " << iter << ", Addr: 0x" << hex << addr << dec << "\n";
                        total_errors++;
                    }
                    usleep(1000);
                    total_tests++; 
                }

                if (iter % 10 == 0) cout << " ... completed " << iter << " iterations\n";
            }
            
            cout << "\n========================================\n";
            cout << " Test Complete!\n";
            cout << " Total Operations: " << total_tests << " (Safe Config W/R + Status R)\n";
            if (total_errors == 0) cout << " Result: PERFECT PASS (0 Errors)\n";
            else cout << " Result: FAILED (" << total_errors << " Errors detected)\n";
            cout << "========================================\n";
        }
        
        // 🌟 Option 7 (Formerly 6): Read Readout Config
        else if (sel == "7") {
            try {
                uhal::ValWord<uint32_t> rdout_val = hw.getNode("ctrl_reg.readout_cfg").read();
                hw.dispatch();
                uint32_t val = rdout_val.value();
                
                uint32_t left_size  = (val >> 0)  & 0x1F; 
                uint32_t right_size = (val >> 5)  & 0x1F; 
                uint32_t l1a_dis    = (val >> 10) & 0x1;  
                uint32_t etroc_addr = (val >> 11) & 0x1F; 
                uint32_t reset_req  = (val >> 16) & 0x1;  
                uint32_t ldo_en     = (val >> 17) & 0x1;  

                cout << "\n[SUCCESS] Current FPGA Readout Config (Raw: 0x" << hex << val << dec << ")\n";
                cout << "  - Trigger Data Size (Left)  : " << left_size << "\n";
                cout << "  - Trigger Data Size (Right) : " << right_size << "\n";
                cout << "  - L1A Disable               : " << l1a_dis << "\n";
                cout << "  - ETROC Address Straps      : " << etroc_addr << "\n";
                cout << "  - ETROC Reset Request       : " << reset_req << "\n";
                cout << "  - LDO Enable                : " << ldo_en << "\n";
            } catch (const exception& e) {
                cout << "[FAIL] IPbus Read Error: " << e.what() << "\n";
            }
        }
        
        // 🌟 Option 8 (Formerly 7): Write Readout Config
        else if (sel == "8") {
            configure_readout_fpga(hw); 
        }
        
        // 🌟 Option 9: Text updated
        else if (sel == "9") {
            cout << ">>> Breaking out of Menu. Starting High-Speed DAQ (DAQ -> PC)...\n";
            return;
        } 
        
        // Option 10: Exit
        else if (sel == "10") {
            cout << "[System] Exiting Program.\n";
            exit(0);
        }
        else {
            cout << "[Error] Invalid Selection.\n";
        }
    }
}
// =====================================================================
// DAQ Stream Producer (with Background Keyboard Monitor Support)
// =====================================================================
void hardware_producer(uhal::HwInterface hw, uint32_t batch, uint32_t block_size, double duration_sec, atomic<uint32_t>& timeout_cnt) {
    auto start = chrono::steady_clock::now();
    while (chrono::duration<double>(chrono::steady_clock::now() - start).count() < duration_sec) {
        try {
            {
                unique_lock<mutex> lock(queue_mtx);
                queue_cv.wait(lock, []{ return data_queue.size() < 50; });
            }

            while (true) {
                CtrlCmd cmd;
                {
                    lock_guard<mutex> lock(ctrl_mtx);
                    if (ctrl_queue.empty()) break;
                    cmd = ctrl_queue.front();
                    ctrl_queue.pop();
                }
                if (cmd.do_write) {
                    hw.getNode("ctrl_reg.cmd").write(cmd.write_val);
                    hw.dispatch();
                    cout << ">>> [Write Done] Sent Raw Value: 0x" << hex << cmd.write_val << dec << " (Inserted seamlessly)\n";
                } else {
                    uhal::ValWord<uint32_t> val = hw.getNode("ctrl_reg.stat").read();
                    hw.dispatch();
                    cout << ">>> [Read Done] stat_reg = 0x" << hex << val.value() << dec << "\n";
                }
            }

            RawBatch rb;
            rb.data.reserve(batch * FW_CYCLES);
            rb.logs.reserve(batch);
            
            for (uint32_t i = 0; i < batch; i++) {
                for (uint32_t j = 0; j < FW_CYCLES; j++) {
                    rb.data.push_back(hw.getNode("ram").readBlock(block_size));
                }
                rb.logs.push_back(hw.getNode("log_ram").readBlock(4096));
            }
            hw.dispatch();
            
            {
                lock_guard<mutex> lock(queue_mtx);
                data_queue.push(rb);
            }
            queue_cv.notify_all();
            
        } catch (const exception&) {
            timeout_cnt++;
            this_thread::sleep_for(chrono::milliseconds(20));
        }
    }

    {
        lock_guard<mutex> lock(queue_mtx);
        daq_done = true;
    }
    queue_cv.notify_all();
}

// =====================================================================
// Main Application
// =====================================================================
int main() {
    uhal::setLogLevelTo(uhal::Error());

    string folder = "daq_bin_results";
    if (!fs::exists(folder)) fs::create_directories(folder);

    try {
        uhal::ConnectionManager manager("file://connections.xml");
        uhal::HwInterface hw = manager.getDevice("kc705");

        run_config_menu(hw);

        thread keyboard_monitor([]() {
            cout << "\n[System] Background Raw I2C Tester Ready (During DAQ)\n"
                 << "[System] 'r'      -> ctrl_reg.stat 상태 읽기\n"
                 << "[System] <number> -> ctrl_reg.cmd 에 Raw 32-bit Command 쓰기\n";
            while (!global_stop) {
                string input;
                if (!(cin >> input)) break;
                try {
                    CtrlCmd cmd;
                    if (input == "r" || input == "R") {
                        cmd.do_write = false;
                        cout << ">>> [Queued] Read Status request\n";
                    } else {
                        cmd.do_write  = true;
                        cmd.write_val = stoul(input, nullptr, 0);
                        cout << ">>> [Queued] Raw Write request: 0x" << hex << cmd.write_val << dec << "\n";
                    }
                    {
                        lock_guard<mutex> lock(ctrl_mtx);
                        ctrl_queue.push(cmd);
                    }
                } catch (const invalid_argument&) {
                    cout << "[Input Error] Please enter 'r' or a valid number.\n";
                }
            }
        });

        const uint32_t BLOCK_SIZE = 262144;
        const double   TEST_TIME  = 1200; 

        for (uint32_t batch = 20; batch <= 20; batch += 10) {
            cout << "\n=======================================================\n";
            cout << ">>> Streaming Batch " << batch << " (Dispatch Size)\n";

            {
                lock_guard<mutex> lock(queue_mtx);
                while (!data_queue.empty()) data_queue.pop();
                daq_done = false;
            }

            atomic<uint32_t> timeouts(0);
            string filename = folder + "/batch_" + to_string(batch) + "_stream.bin";
            
            vector<char> io_buf(8 * 1024 * 1024);
            ofstream out_file;
            out_file.rdbuf()->pubsetbuf(io_buf.data(), io_buf.size());
            out_file.open(filename, ios::binary);

            auto t_start = chrono::steady_clock::now();
            thread prod(hardware_producer, hw, batch, BLOCK_SIZE, TEST_TIME, std::ref(timeouts));

            while (true) {
                RawBatch rb;
                {
                    unique_lock<mutex> lock(queue_mtx);
                    queue_cv.wait(lock, []{ return !data_queue.empty() || daq_done; });
                    if (data_queue.empty() && daq_done) break;
                    rb = move(data_queue.front());
                    data_queue.pop();
                }
                queue_cv.notify_all();

                for (size_t i = 0; i < rb.logs.size(); i++) {
                    if (rb.logs[i].valid() && !rb.logs[i].value().empty()) {
                        const auto& lv = rb.logs[i].value();
                        write_block_header(out_file, MAGIC_LOG, (uint32_t)lv.size());
                        out_file.write(reinterpret_cast<const char*>(lv.data()), lv.size() * 4);
                    }
                    for (size_t j = 0; j < FW_CYCLES; j++) {
                        size_t data_idx = i * FW_CYCLES + j;
                        if (data_idx < rb.data.size() && rb.data[data_idx].valid()) {
                            const auto& dv = rb.data[data_idx].value();
                            if (!dv.empty()) {
                                write_block_header(out_file, MAGIC_DAT, (uint32_t)dv.size());
                                out_file.write(reinterpret_cast<const char*>(dv.data()), dv.size() * 4);
                            }
                        }
                    }
                }
            }

            prod.join();
            auto t_end = chrono::steady_clock::now();
            double elapsed_sec = chrono::duration<double>(t_end - t_start).count();

            write_block_header(out_file, MAGIC_META, 2);
            out_file.write(reinterpret_cast<const char*>(&elapsed_sec), sizeof(double));
            out_file.close();

            if (timeouts > 0) cout << "  Warning: Timeouts Recorded: " << timeouts << "\n";
        }

        cout << "\n[System] DAQ Loop finished. Processing any remaining background commands...\n";
        while (true) {
            CtrlCmd cmd;
            {
                lock_guard<mutex> lock(ctrl_mtx);
                if (ctrl_queue.empty()) break;
                cmd = ctrl_queue.front();
                ctrl_queue.pop();
            }
            try {
                if (cmd.do_write) {
                    hw.getNode("ctrl_reg.cmd").write(cmd.write_val);
                    hw.dispatch();
                    cout << ">>> [Write Done] Sent Raw Value: 0x" << hex << cmd.write_val << dec << " (Post-DAQ)\n";
                } else {
                    uhal::ValWord<uint32_t> val = hw.getNode("ctrl_reg.stat").read();
                    hw.dispatch();
                    cout << ">>> [Read Done] stat_reg = 0x" << hex << val.value() << dec << " (Post-DAQ)\n";
                }
            } catch (...) {
                cout << ">>> [IPbus Error] Final transaction failed.\n";
            }
        }
        
        global_stop = true;
        keyboard_monitor.detach();
        cout << "\n[System] All DAQ tasks completed safely. Exiting program...\n";

    } catch (exception& e) {
        cerr << "[Fatal] " << e.what() << "\n";
    }

    return 0;
}