#include <iostream>
#include <vector>
#include <iomanip>
#include "uhal/uhal.hpp"

int main() {
    // 1. 로그 레벨 설정 (에러만 출력하여 콘솔을 깔끔하게 유지)
    uhal::setLogLevelTo(uhal::Error());

    // 2. 연결 설정 ("connections.xml" 파일의 "kc705" 디바이스 사용)
    uhal::ConnectionManager manager("file://connections.xml");
    uhal::HwInterface hw = manager.getDevice("kc705");

    try {
        std::cout << ">>> Queuing all transactions..." << std::endl;

        // ====================================================
        // [Phase 1] 트랜잭션 예약 (Queueing)
        // hw.dispatch()를 호출하기 전까지는 실제로 전송되지 않고 쌓입니다.
        // ====================================================

        // ----------------------------------------------------
        // [1] Register Test
        // ----------------------------------------------------
        // (A) Status 레지스터 읽기
        uhal::ValWord<uint32_t> user_status = hw.getNode("status").read();

        // (B) Single Register (XML ID: "reg") 쓰기 및 읽기
        hw.getNode("reg").write(0xDEADBEEF);
        uhal::ValWord<uint32_t> reg_val = hw.getNode("reg").read();


        // ----------------------------------------------------
        // [2] RMW (Read-Modify-Write) Test
        // ----------------------------------------------------
        // XML에서 "reg" 노드의 실제 주소를 가져옵니다. (address_table.xml에 따름)
        uint32_t reg_addr = hw.getNode("reg").getAddress();
        
        uint32_t and_mask = 0xFFFF0000; // 하위 16비트 지우기
        uint32_t or_mask  = 0x0000CAFE; // 하위 16비트에 CAFE 채우기

        // RMW 수행 (Client 객체를 통해 직접 수행)
        hw.getClient().rmw_bits(reg_addr, and_mask, or_mask);

        // 결과 확인을 위해 다시 읽기
        uhal::ValWord<uint32_t> rmw_result = hw.getNode("reg").read();


        // ----------------------------------------------------
        // [3] RAM Block Test
        // ----------------------------------------------------
        std::vector<uint32_t> write_data = {0x1, 0x2, 0x3, 0x4, 0x5};
        
        // 블록 쓰기 (Burst Write)
        hw.getNode("ram").writeBlock(write_data);
        
        // 블록 읽기 (Burst Read)
        uhal::ValVector<uint32_t> read_ram = hw.getNode("ram").readBlock(write_data.size());


        // ----------------------------------------------------
        // [4] Peephole RAM Test
        // ----------------------------------------------------
        // 테스트할 내부 주소 (0 ~ 1023 사이)
        uint32_t target_addr = 0xA;       
        uint32_t target_val  = 0x55AA55AA;

        // [Step 1] 쓰기: (1) 주소 설정 -> (2) 데이터 쓰기
        hw.getNode("peephole.addr").write(target_addr);
        hw.getNode("peephole.data").write(target_val);

        // [Step 2] 읽기: (1) 주소 재설정 -> (2) 데이터 읽기
        // * 중요: Peephole은 읽기 전에 반드시 주소 포인터를 다시 설정해야 합니다.
        hw.getNode("peephole.addr").write(target_addr);
        uhal::ValWord<uint32_t> peephole_res = hw.getNode("peephole.data").read();


        // ====================================================
        // [Phase 2] 전송 및 실행 (Dispatch)
        // ====================================================
        std::cout << ">>> Sending all packets (Dispatch)..." << std::endl;
        
        // 큐에 쌓인 모든 명령을 FPGA로 전송하고 응답을 받습니다.
        hw.dispatch(); 
        
        std::cout << ">>> Transaction Complete! Checking results...\n" << std::endl;


        // ====================================================
        // [Phase 3] 결과 검증 (Verification)
        // ====================================================

        // --- [1] Register Check ---
        std::cout << "--- [1] Register Test ---" << std::endl;
        std::cout << "User Status          : 0x" << std::hex << user_status.value() << std::endl;
        std::cout << "Single Reg Read      : 0x" << std::hex << reg_val.value() << std::endl;
        
        if (reg_val.value() == 0xDEADBEEF) std::cout << ">> PASS" << std::endl;
        else std::cout << ">> FAIL (Expected: 0xDEADBEEF)" << std::endl;
        std::cout << std::endl;


        // --- [2] RMW Check ---
        std::cout << "--- [2] RMW Test ---" << std::endl;
        std::cout << "RMW Result           : 0x" << std::hex << rmw_result.value() << std::endl;
        
        if (rmw_result.value() == 0xDEADCAFE) std::cout << ">> PASS" << std::endl;
        else std::cout << ">> FAIL (Expected: 0xDEADCAFE)" << std::endl;
        std::cout << std::endl;


        // --- [3] RAM Check ---
        std::cout << "--- [3] RAM Block Test ---" << std::endl;
        bool ram_success = true;
        for(size_t i=0; i<read_ram.size(); ++i) {
            if (write_data[i] != read_ram[i]) {
                std::cout << "RAM[" << std::dec << i << "] Mismatch! W:0x" << std::hex << write_data[i] 
                          << " R:0x" << read_ram[i] << std::endl;
                ram_success = false;
            }
        }
        if(ram_success) std::cout << ">> PASS" << std::endl;
        else std::cout << ">> FAIL" << std::endl;
        std::cout << std::endl;


        // --- [4] Peephole Check ---
        std::cout << "--- [4] Peephole Test ---" << std::endl;
        std::cout << "Target Addr (Internal): 0x" << std::hex << target_addr << std::endl;
        std::cout << "Write Data            : 0x" << std::hex << target_val << std::endl;
        std::cout << "Read Data             : 0x" << std::hex << peephole_res.value() << std::endl;
        
        if (target_val == peephole_res.value()) std::cout << ">> PASS" << std::endl;
        else std::cout << ">> FAIL" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Communication Error: " << e.what() << std::endl;
    }

    return 0;
}
