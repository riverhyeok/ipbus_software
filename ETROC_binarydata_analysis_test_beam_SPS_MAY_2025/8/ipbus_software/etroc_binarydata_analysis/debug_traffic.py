import uhal
import time

if __name__ == "__main__":
    uhal.setLogLevelTo(uhal.LogLevel.ERROR)
    
    # 연결 설정 (본인의 환경에 맞게 유지)
    uri = "ipbusudp-2.0://192.168.200.16:50001"
    address_table = "file://address_table.xml" 
    hw = uhal.getDevice("fpga", uri, address_table)

    print("------------------------------------------------")
    print(" IPbus Traffic Generator for ILA Verification")
    print("------------------------------------------------")

    # 테스트 케이스 정의 (주소 이름, 쓸 데이터)
    # address_table.xml에 정의된 id를 사용합니다.
    test_vectors = [
        ("csr.ctrl", 0x12345678), # Case 1: Control Reg (0x0)
        ("reg",      0xDEADBEEF), # Case 2: Target Reg  (0x2)
        ("ram",      0xAABBCCDD)  # Case 3: RAM         (0x1000)
    ]

    for node_id, write_val in test_vectors:
        print(f"\n[Test] Target: {node_id}")
        
        try:
            # 1. 쓰기 (Write)
            print(f"  -> Writing {hex(write_val)}...")
            hw.getNode(node_id).write(write_val)
            hw.dispatch() # 전송!
            
            # 2. 읽기 (Read Back) - 검증용
            # val = hw.getNode(node_id).read()
            # hw.dispatch()
            print("  -> Success! (Ack received)")

        except Exception as e:
            print("  -> FAILED! (Timeout or Error)")
            print(f"     (Check ILA now: Did address change to target?)")
        
        # ILA에서 눈으로 확인하기 좋게 1초 쉽니다.
        time.sleep(1)

    print("\n------------------------------------------------")
    print("Test Complete.")
