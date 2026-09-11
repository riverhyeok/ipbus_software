import uhal

# 0. 로그 설정 (에러만 표시)
uhal.setLogLevelTo(uhal.LogLevel.ERROR)

# ---------------------------------------------------------
# [질문하신 부분 1] connection file 연결
# 튜토리얼: ConnectionManager manager("file://path/to/connections.xml");
# ---------------------------------------------------------
manager = uhal.ConnectionManager("file://connections.xml")

# ---------------------------------------------------------
# [질문하신 부분 2] HwInterface 생성
# 튜토리얼: HwInterface hw = manager.getDevice("kc705");
# ---------------------------------------------------------
# connections.xml 안에 있는 id="kc705"를 가져와서 'hw'라는 변수에 담습니다.
# 이 'hw'가 바로 HwInterface입니다.
hw = manager.getDevice("kc705")

# ---------------------------------------------------------
# 레지스터 쓰고 읽기
# ---------------------------------------------------------
# address_table.xml에 있는 id="reg" (주소 0x2)를 사용
print("Writing 1 to register 'reg'...")
hw.getNode("reg").write(1)

print("Reading back from register 'reg'...")
reg_value = hw.getNode("reg").read()

# ---------------------------------------------------------
# 전송 (Dispatch) - 여기서 Timeout이 나면 네트워크 문제입니다.
# ---------------------------------------------------------
try:
    hw.dispatch()
    print("Reg Value =", reg_value.value()) # 결과 출력
except Exception as e:
    print("\n[FAIL] Timeout Error!")
    print("원인: PC가 FPGA의 답장을 못 받고 있습니다.")
    print("해결: 'ping 192.168.200.16'이 성공해야만 이 코드가 작동합니다.")
