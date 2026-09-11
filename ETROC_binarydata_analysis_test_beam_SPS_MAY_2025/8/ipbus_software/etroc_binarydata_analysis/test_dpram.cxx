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
// 🛠️ I2C Communication Helpers (Fix: C++ Bit-Packing matches VHDL)
// =====================================================================
uint32_t make_i2c_cmd(uint8_t op_mode, uint8_t multi_num, uint16_t reg_addr, uint8_t data) {
    uint32_t cmd = 0;
    // VHDL: command_wdata <= ctrl(25 downto 18);
    cmd |= (static_cast<uint32_t>(data & 0xFF) << 18);
    // VHDL: command_regaddr <= ctrl(17 downto 2);
    cmd |= (static_cast<uint32_t>(reg_addr & 0xFFFF) << 2);
    // VHDL: command_op_mode <= ctrl(1 downto 0);
    cmd |= (static_cast<uint32_t>(op_mode & 0x03));
    return cmd;
}

bool i2c_write_reg(uhal::HwInterface& hw, uint16_t reg_addr, uint8_t data) {
    uint32_t cmd = make_i2c_cmd(0, 1, reg_addr, data); // 0 = Write Only
    hw.getNode("ctrl_reg.cmd").write(cmd); // 🌟 노드 이름 변경
    hw.dispatch();

    uint32_t val;
    int timeout = 10000; // ~1s @ 100us intervals
    do {
        uhal::ValWord<uint32_t> stat = hw.getNode("ctrl_reg.stat").read(); // 🌟 노드 이름 변경
        hw.dispatch();
        val = stat.value();
        
        if (--timeout == 0) {
            cerr << "[i2c error] Write Timeout: Hardware busy stuck high.\n";
            return false;
        }
        usleep(100);
    } while (val & 0x01); // Bit 0 (busy)가 0이 될 때까지 대기

    bool is_nack = (val & 0x04) != 0; // Bit 2: ack_hold (0=ACK, 1=NACK)
    return !is_nack; 
}

bool i2c_read_reg(uhal::HwInterface& hw, uint16_t reg_addr, uint8_t& out_data) {
    uint32_t cmd = make_i2c_cmd(1, 1, reg_addr, 0); // 1 = Read Only
    hw.getNode("ctrl_reg.cmd").write(cmd); // 🌟 노드 이름 변경
    hw.dispatch();

    uint32_t val;
    int timeout = 10000; // ~1s @ 100us intervals
    do {
        uhal::ValWord<uint32_t> stat = hw.getNode("ctrl_reg.stat").read(); // 🌟 노드 이름 변경
        hw.dispatch();
        val = stat.value();
        
        if (--timeout == 0) {
            cerr << "[i2c error] Read Timeout: Hardware busy stuck high.\n";
            return false;
        }
        usleep(100);
    } while (val & 0x01); // Bit 0 (busy)가 0이 될 때까지 대기

    bool is_nack = (val & 0x04) != 0;
    out_data = (val >> 3) & 0xFF; // Bit 10:3: rdval_hold 추출
    return !is_nack;
}

// =====================================================================
// 🖥️ Configuration Interactive Menu (Pre-DAQ)
// =====================================================================
void run_config_menu(uhal::HwInterface& hw) {
    while (true) {
        cout << "\n=======================================================\n";
        cout << "   ETROC2 Configuration Menu (I2C via IPbus)\n";
        cout << "=======================================================\n";
        cout << " 1. Apply Target Run Configs (320Mbps & FC Align/L1A)\n";
        cout << " 2. Read arbitrary Register\n";
        cout << " 3. Write arbitrary Register\n";
        cout << " 4. Proceed to High-Speed DAQ Mode\n";
        cout << " 5. Quit Program\n";
        cout << "-------------------------------------------------------\n";
        cout << "Select Option > ";

        string sel;
        cin >> sel;

        if (sel == "1") {
            cout << "\n>>> Applying Target Run Configurations...\n";
            
            // 1. PeriCfg19 (0x0013): 320Mbps (serRateRight/Left = 0)
            if (i2c_write_reg(hw, 0x0013, 0x42)) {
                cout << "[OK] PeriCfg19 (0x0013) set to 0x42\n";
            } else {
                cout << "[FAIL] PeriCfg19 (0x0013) Write NACK!\n";
            }

            // 2. PeriCfg18 (0x0012): Periodic L1A (0x2), fcDataDelayEn (1), fcClkDelayEn (1)
            // Note: 0x58 corrects the previous 0x78 to match 2'b10 for onChipL1AConf
            if (i2c_write_reg(hw, 0x0012, 0x58)) {
                cout << "[OK] PeriCfg18 (0x0012) set to 0x58\n";
            } else {
                cout << "[FAIL] PeriCfg18 (0x0012) Write NACK!\n";
            }
            
            // 향후 image_ca996a.png의 다른 레지스터(예: PLL 설정 등) 추가가 필요하다면 
            // 아래에 if (i2c_write_reg(hw, 주소, 값)) 형태로 계속 나열하시면 됩니다.
        }
        else if (sel == "2") {
            cout << "\n> Enter 16-bit Address (Hex, e.g. 0013): ";
            uint16_t addr;
            cin >> hex >> addr >> dec;

            uint8_t read_val = 0;
            if (i2c_read_reg(hw, addr, read_val)) {
                cout << "[SUCCESS] Read Reg [0x" << hex << addr << "] = 0x" << (int)read_val << dec << "\n";
            } else {
                cout << "[FAIL] Read Error (NACK received)\n";
            }
        } 
        else if (sel == "3") {
            cout << "\n> Enter 16-bit Address (Hex, e.g. 0013): ";
            uint16_t addr;
            cin >> hex >> addr >> dec;

            cout << "> Enter 8-bit Data to Write (Hex, e.g. 42): ";
            uint16_t write_val;
            cin >> hex >> write_val >> dec;

            if (i2c_write_reg(hw, addr, (uint8_t)write_val)) {
                cout << "[SUCCESS] Write Reg [0x" << hex << addr << "] with 0x" << write_val << dec << "\n";
            } else {
                cout << "[FAIL] Write Error (NACK received)\n";
            }
        } 
        else if (sel == "4") {
            cout << ">>> Breaking out of Menu. Starting High-Speed DAQ...\n";
            return;
        } 
        else if (sel == "5") {
            cout << "[System] Exiting Program.\n";
            exit(0);
        }
        else {
            cout << "[Error] Invalid Selection.\n";
        }
    }
}

// =====================================================================
// 🚀 DAQ Stream Producer (with Background Keyboard Monitor Support)
// =====================================================================
void hardware_producer(uhal::HwInterface hw, uint32_t batch, uint32_t block_size, double duration_sec, atomic<uint32_t>& timeout_cnt) {
    auto start = chrono::steady_clock::now();
    while (chrono::duration<double>(chrono::steady_clock::now() - start).count() < duration_sec) {
        try {
            {
                unique_lock<mutex> lock(queue_mtx);
                queue_cv.wait(lock, []{ return data_queue.size() < 50; });
            }

            // ── 1. 키보드 명령이 큐에 있다면 IPbus 통신 사이에 살짝 껴넣기 ──
            while (true) {
                CtrlCmd cmd;
                {
                    lock_guard<mutex> lock(ctrl_mtx);
                    if (ctrl_queue.empty()) break;
                    cmd = ctrl_queue.front();
                    ctrl_queue.pop();
                }
                
                if (cmd.do_write) {
                    hw.getNode("ctrl_reg.cmd").write(cmd.write_val); // 🌟 노드 이름 변경
                    hw.dispatch();
                    cout << ">>> [Write Done] Sent Raw Value: 0x" << hex << cmd.write_val << dec << " (Inserted seamlessly)\n";
                } else {
                    uhal::ValWord<uint32_t> val = hw.getNode("ctrl_reg.stat").read(); // 🌟 노드 이름 변경
                    hw.dispatch();
                    cout << ">>> [Read Done] stat_reg = 0x" << hex << val.value() << dec << "\n";
                }
            }

            // ── 2. 거대 데이터(Batch) 읽어오기 ──
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
// 🏁 Main Application
// =====================================================================
int main() {
    uhal::setLogLevelTo(uhal::Error());
    string folder = "daq_bin_results";
    if (!fs::exists(folder)) fs::create_directories(folder);

    try {
        uhal::ConnectionManager manager("file://connections.xml");
        uhal::HwInterface hw = manager.getDevice("kc705");
        
        // 1. 초기 I2C Configuration 메뉴 실행
        run_config_menu(hw);

        // 2. DAQ 중 백그라운드 키보드 인터랙션 스레드 실행
        thread keyboard_monitor([]() {
            cout << "\n[System] Background Raw I2C Tester Ready (During DAQ)\n"
                 << "[System] 'r'      -> stat 상태 읽기\n"
                 << "[System] <number> -> cmd 에 Raw 32-bit Command 쓰기\n";
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

        // =====================================================================
        // High-Speed DAQ Stream Loop
        // =====================================================================
        const uint32_t BLOCK_SIZE = 262144;
        const double   TEST_TIME  = 10;

        for (uint32_t batch = 20; batch <= 60; batch += 10) {
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
        
        // =====================================================================
        // 프로그램 종료 직전, 큐에 남아있는 마지막 제어 명령들 모두 소화
        // =====================================================================
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
                    hw.getNode("ctrl_reg.cmd").write(cmd.write_val); // 🌟 노드 이름 변경
                    hw.dispatch();
                    cout << ">>> [Write Done] Sent Raw Value: 0x" << hex << cmd.write_val << dec << " (Post-DAQ)\n";
                } else {
                    uhal::ValWord<uint32_t> val = hw.getNode("ctrl_reg.stat").read(); // 🌟 노드 이름 변경
                    hw.dispatch();
                    cout << ">>> [Read Done] stat_reg = 0x" << hex << val.value() << dec << " (Post-DAQ)\n";
                }
            } catch (...) {
                cout << ">>> [IPbus Error] Final transaction failed.\n";
            }
        }

        // ─── 정상 종료 처리 ───
        global_stop = true;
        keyboard_monitor.detach();
        cout << "\n[System] All DAQ tasks completed safely. Exiting program...\n";
        
    } catch (exception& e) {
        cerr << "[Fatal] " << e.what() << "\n";
    }
    return 0;

}
