#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include "uhal/uhal.hpp"

// =========================================================================
// [튜닝 포인트] 한 번에 읽어올 데이터 크기
// =========================================================================
const uint32_t READ_SIZE = 65536; 

int main(int argc, char* argv[])
{
    // 1. 로그 끄기
    uhal::setLogLevelTo(uhal::Error());

    try {
        // 2. 연결 설정
        std::string connectionFilePath = "file://connections.xml";
        std::string deviceId = "kc705";
        
        uhal::ConnectionManager manager(connectionFilePath);
        uhal::HwInterface hw = manager.getDevice(deviceId);
        
        // XML에 정의된 FIFO 노드 가져오기
        const uhal::Node& fifoNode = hw.getNode("fifo");

        std::cout << "==================================================" << std::endl;
        std::cout << "      Ultra-High Speed FIFO Reader (DAQ Mode)     " << std::endl;
        std::cout << "==================================================" << std::endl;
        std::cout << "Target Node : " << fifoNode.getId() << std::endl;
        std::cout << "Read Mode   : Controlled by XML (non-incremental)" << std::endl;
        std::cout << "Block Size  : " << READ_SIZE << " words (" << (READ_SIZE * 4)/1024.0 << " KB)" << std::endl;
        std::cout << "--------------------------------------------------" << std::endl;
        std::cout << "Starting measurement... (Press Ctrl+C to stop)" << std::endl;

        // 3. 속도 측정을 위한 변수들
        auto start_time = std::chrono::steady_clock::now();
        auto last_print_time = start_time;
        uint64_t total_bytes_read = 0;
        uint64_t interval_bytes_read = 0;

        while(true) {
            // ============================================================
            // [수정 1] 인자 제거 (XML 설정을 따름)
            // ============================================================
            uhal::ValVector<uint32_t> result = fifoNode.readBlock(READ_SIZE);
            
            // [전송]
            hw.dispatch(); 

            // 통계 업데이트
            uint64_t bytes_just_read = result.size() * 4; // 32-bit = 4 bytes
            total_bytes_read += bytes_just_read;
            interval_bytes_read += bytes_just_read;

            // ============================================================
            // [리포트] 1초마다 속도 출력
            // ============================================================
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double> diff = now - last_print_time;

            if (diff.count() >= 1.0) { // 1초 경과
                // [수정 2] 콤마(,) 제거: 1000000.0
                double mbps = (interval_bytes_read * 8.0) / diff.count() / 1000000.0;
                double mbs  = (interval_bytes_read) / diff.count() / 1024.0 / 1024.0;
                
                uint32_t last_data = 0;
                if(result.size() > 0) last_data = result.value().back();

                std::cout << "[Speed] " 
                          << std::fixed << std::setprecision(2) << mbps << " Mbps "
                          << "(" << mbs << " MB/s) "
                          << "| Last Data: " << std::dec << last_data
                          << std::endl;

                last_print_time = now;
                interval_bytes_read = 0;
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "\n[ERROR] Exception caught: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
