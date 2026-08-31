import uhal

if __name__ == "__main__":
    # 1. 로그 레벨 설정 (에러만 보기)
    uhal.setLogLevelTo(uhal.LogLevel.ERROR)

    # 2. 하드웨어 연결 정의
    # 형식: ipbusudp-2.0://[FPGA_IP]:50001
    uri = "ipbusudp-2.0://192.168.200.16:50001"
    
    # 주소 테이블 파일 (현재 폴더에 있는 파일 이름)
    address_table = "file://address_table.xml"

    print(f"Connecting to {uri} ...")
    
    try:
        # 디바이스 객체 생성
        hw = uhal.getDevice("my_device", uri, address_table)

        # 3. 레지스터 읽기 테스트
        # 보통 가장 기본 레지스터인 "CSR.CTRL"이나 id 등을 읽습니다.
        # 만약 에러가 나면 address_table.xml 안에 있는 노드 이름 중 하나로 바꾸세요.
        # 예: hw.getNode("csr.ctrl").read() -> hw.getNode("YOUR_NODE_NAME").read()
        
        # 여기서는 단순히 연결이 되는지만 확인하기 위해 노드 확인 없이 dispatch만 시도해봅니다.
        # (Ping이 된다면 이 단계는 성공해야 합니다)
        hw.dispatch()
        print("Success! Connection established via IPbus.")

    except Exception as e:
        print("Failed to connect.")
        print("Error:", e)

