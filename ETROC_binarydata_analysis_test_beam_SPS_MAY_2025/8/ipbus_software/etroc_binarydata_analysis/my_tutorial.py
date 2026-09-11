library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;
use work.ipbus.all;

entity ipbus_dpram is
    generic(
        ADDR_WIDTH : positive := 17
    );
    port(
        clk : in std_logic;
        rst : in std_logic;
        ipb_in : in ipb_wbus;
        ipb_out : out ipb_rbus;
        
        rclk : in std_logic;
        we : in std_logic;
        d : in std_logic_vector(31 downto 0);
        
        dpram_full : out std_logic;
        debug_drop_count : out std_logic_vector(31 downto 0);
        
        event_we : out std_logic;
        event_data : out std_logic_vector(31 downto 0);
        cycle_done : out std_logic
    );
end ipbus_dpram;

architecture rtl of ipbus_dpram is
    constant MAIN_RAM_SIZE : integer := 131072; 
    
    type ram_type is array(2**ADDR_WIDTH-1 downto 0) of std_logic_vector(31 downto 0);
    signal ram : ram_type := (others => (others => '0'));
    attribute ram_style : string;
    attribute ram_style of ram : signal is "block";
    attribute cascade_height : integer;
    attribute cascade_height of ram : signal is 1;
    signal ram_dout : std_logic_vector(31 downto 0) := (others => '0');
    signal underflow_flag : std_logic := '0';

    -- N+1 bit pointer (18-bit). MSB is for lap around identification
    signal ptr_write, ptr_read : unsigned(ADDR_WIDTH downto 0) := (others => '0');
    
    signal ptr_w_gray, ptr_r_gray : unsigned(ADDR_WIDTH downto 0) := (others => '0');
    signal ptr_w_gray_s1, ptr_w_gray_s2 : unsigned(ADDR_WIDTH downto 0) := (others => '0');
    signal ptr_r_gray_s1, ptr_r_gray_s2 : unsigned(ADDR_WIDTH downto 0) := (others => '0');
    
    signal ptr_w_sync_bin, ptr_r_sync_bin : unsigned(ADDR_WIDTH downto 0) := (others => '0');

    -- Virtual pointer to prevent deadlock (always runs to generate cycle_done)
    signal virtual_ptr : unsigned(ADDR_WIDTH-1 downto 0) := (others => '0');

    signal drop_cnt_local : unsigned(13 downto 0) := (others => '0');
    signal drop_start_addr : std_logic_vector(16 downto 0) := (others => '0');
    signal is_full_sig    : std_logic := '0';
    signal was_full       : std_logic := '0';
    signal ack : std_logic;
    
    signal lap_cnt : unsigned(3 downto 0) := (others => '0');
    signal pending_lap : std_logic := '0';
    signal err_in_lap : std_logic := '0';

    function bin2gray(bin : unsigned) return unsigned is
    begin
        return bin xor ('0' & bin(bin'high downto 1));
    end function;

    function gray2bin(gray : unsigned) return unsigned is
        variable bin : unsigned(gray'range);
    begin
        bin(gray'high) := gray(gray'high);
        for i in gray'high-1 downto 0 loop
            bin(i) := bin(i+1) xor gray(i);
        end loop;
        return bin;
    end function;

begin

    -- BRAM Read Domain
    process(clk)
    begin
        if rising_edge(clk) then
            ram_dout <= ram(to_integer(ptr_read(ADDR_WIDTH-1 downto 0)));
        end if;
    end process;

    -- PC Read Domain (IPbus 125MHz)
    process(clk)
    begin
        if rising_edge(clk) then
            if rst = '1' then
                ptr_read <= (others => '0');
                ptr_r_gray <= (others => '0');
                ack <= '0';
                underflow_flag <= '0';
            else
                ptr_r_gray <= bin2gray(ptr_read);
                
                ptr_w_gray_s1 <= ptr_w_gray;
                ptr_w_gray_s2 <= ptr_w_gray_s1;
                ptr_w_sync_bin <= gray2bin(ptr_w_gray_s2);
                
                ack <= ipb_in.ipb_strobe and not ack;
                
                if ipb_in.ipb_strobe = '1' and ack = '0' then
                    -- Completely Empty (Underflow) condition
                    if ptr_read = ptr_w_sync_bin then
                        underflow_flag <= '1';
                    else
                        underflow_flag <= '0';
                        ptr_read <= ptr_read + 1; 
                    end if;
                end if;
            end if;
        end if;
    end process;
    
    ipb_out.ipb_ack <= ack;
    ipb_out.ipb_err <= '0';
    ipb_out.ipb_rdata <= x"00000000" when underflow_flag = '1' else ram_dout;
    debug_drop_count <= std_logic_vector(resize(drop_cnt_local, 32));

    -- ETROC Write Domain (40MHz)
    -- N+1 bit Full condition
    is_full_sig <= '1' when (ptr_write(ADDR_WIDTH) /= ptr_r_sync_bin(ADDR_WIDTH)) and 
                            (ptr_write(ADDR_WIDTH-1 downto 0) = ptr_r_sync_bin(ADDR_WIDTH-1 downto 0)) else '0';
    dpram_full <= is_full_sig;

    process(rclk)
    begin
        if rising_edge(rclk) then
            ptr_w_gray <= bin2gray(ptr_write);
            
            ptr_r_gray_s1 <= ptr_r_gray;
            ptr_r_gray_s2 <= ptr_r_gray_s1;
            ptr_r_sync_bin <= gray2bin(ptr_r_gray_s2);
            
            event_we <= '0';
            cycle_done <= '0';

            if we = '1' then
                virtual_ptr <= virtual_ptr + 1;
                
                if virtual_ptr = 0 then
                    if err_in_lap = '1' then pending_lap <= '1'; end if;
                    err_in_lap <= '0'; 
                    if lap_cnt = 9 then lap_cnt <= (others => '0'); else lap_cnt <= lap_cnt + 1; end if;
                end if;

                if virtual_ptr = MAIN_RAM_SIZE - 1 then
                    cycle_done <= '1';
                end if;

                if is_full_sig = '1' then
                    err_in_lap <= '1'; 
                    
                    -- Capture the exact start address of the drop
                    if was_full = '0' then
                        drop_start_addr <= std_logic_vector(ptr_write(ADDR_WIDTH-1 downto 0));
                    end if;

                    if drop_cnt_local /= 16383 then
                        drop_cnt_local <= drop_cnt_local + 1;
                    end if;
                else
                    ram(to_integer(ptr_write(ADDR_WIDTH-1 downto 0))) <= d;
                    ptr_write <= ptr_write + 1;
                end if;
            end if;
            
            -- Transmit error event when full condition is released
            if was_full = '1' and is_full_sig = '0' then
                event_data <= "0" & std_logic_vector(drop_cnt_local) & drop_start_addr;
                event_we <= '1';
                drop_cnt_local <= (others => '0');
            elsif pending_lap = '1' then
                event_data <= "1" & "111111111111111111111111111" & std_logic_vector(lap_cnt);
                event_we <= '1';
                pending_lap <= '0';
            end if;
            
            was_full <= is_full_sig;
        end if;
    end process;
end rtl;
library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;
use work.ipbus.all;

-- ============================================================
-- ipbus_etroc_log
--
-- 동작 개요:
--   1. DPRAM이 CYCLES_PER_BATCH 사이클을 완료하면 is_full='1' 설정
--   2. is_full='1' 동안 새 이벤트 쓰기 차단
--   3. PC가 4096번째 주소(0xFFF)를 읽으면 reset_toggle_clk 반전
--   4. Toggle이 40MHz 도메인에 전파되면 log 상태 초기화 + is_full='0'
--
-- is_full 버그 수정:
--   ▶ Toggle reset을 is_full='1'일 때만 허용
--     → PC가 DEADBEEF 상태(미완료 배치)에서 0xFFF 읽어도 cycle_cnt 리셋 안 됨
--   ▶ ASYNC_REG 속성 추가로 메타스테이빌리티 안전화
-- ============================================================

entity ipbus_etroc_log is
    generic(
        CYCLES_PER_BATCH : positive := 10
    );
    port(
        clk          : in  std_logic;   -- 125MHz IPbus
        rst          : in  std_logic;
        ipb_in_log   : in  ipb_wbus;
        ipb_out_log  : out ipb_rbus;

        rclk         : in  std_logic;   -- 40MHz ETROC
        event_we     : in  std_logic;
        event_data   : in  std_logic_vector(31 downto 0);
        cycle_done   : in  std_logic
    );
end ipbus_etroc_log;

architecture rtl of ipbus_etroc_log is

    -- ── Log RAM (4096 × 32bit) ────────────────────────────────
    type log_array is array(4095 downto 0) of std_logic_vector(31 downto 0);
    shared variable log_ram : log_array := (0 => x"DEADBEEF", others => (others => '0'));
    attribute ram_style : string;
    attribute ram_style of log_ram : variable is "block";

    -- ── 40MHz 도메인 상태 ─────────────────────────────────────
    signal log_ptr      : unsigned(12 downto 0) := to_unsigned(1, 13);
    signal cycle_cnt    : integer range 0 to 255 := 0;
    signal log_overflow : std_logic := '0';
    signal is_full      : std_logic := '0';
    signal batch_counter: unsigned(15 downto 0) := (others => '0');

    -- ── Toggle CDC: 125MHz → 40MHz ────────────────────────────
    -- reset_toggle_clk: 125MHz에서 갱신되는 toggle 신호
    -- 3단계 동기화 후 에지 감지로 is_full 리셋
    signal reset_toggle_clk                      : std_logic := '0';
    signal reset_sync_1, reset_sync_2, reset_sync_3 : std_logic := '0';

    attribute ASYNC_REG : string;
    attribute ASYNC_REG of reset_sync_1 : signal is "TRUE";
    attribute ASYNC_REG of reset_sync_2 : signal is "TRUE";
    -- reset_sync_3은 에지 감지용 이전값 FF → 동일 클럭 도메인이므로 불필요

    -- ── 125MHz 도메인 ─────────────────────────────────────────
    signal q_log    : std_logic_vector(31 downto 0) := (others => '0');
    signal ack_log  : std_logic := '0';

begin

    -- ============================================================
    -- 🔴 40MHz: 이벤트 수신 + 배치 완료 감지
    -- ============================================================
    process(rclk)
    begin
        if rising_edge(rclk) then

            -- Toggle CDC 3단계 동기화
            reset_sync_1 <= reset_toggle_clk;
            reset_sync_2 <= reset_sync_1;
            reset_sync_3 <= reset_sync_2;

            if rst = '1' then
                -- ── 하드웨어 리셋 ──────────────────────────────
                log_ptr       <= to_unsigned(1, 13);
                cycle_cnt     <= 0;
                log_overflow  <= '0';
                is_full       <= '0';
                batch_counter <= (others => '0');
                log_ram(0)    := x"DEADBEEF";

            elsif (reset_sync_2 /= reset_sync_3) and (is_full = '1') then
                -- ── PC가 완료된 배치를 읽고 Toggle 전송 ─────────
                -- is_full='1' 조건 필수:
                --   → PC가 DEADBEEF 상태(미완료)에서 0xFFF 읽어도 무시
                --   → 실제 배치 데이터가 준비된 경우에만 초기화
                log_ptr      <= to_unsigned(1, 13);
                cycle_cnt    <= 0;
                log_overflow <= '0';
                is_full      <= '0';
                log_ram(0)   := x"DEADBEEF";
                -- batch_counter는 누적 유지 (리셋 안 함)

            elsif is_full = '0' then
                -- ── 정상 쓰기 구간 ─────────────────────────────

                -- event_we와 cycle_done은 독립 처리
                -- event_we: overflow/lap 이벤트 기록
                if event_we = '1' then
                    if log_ptr < 4096 then
                        log_ram(to_integer(log_ptr)) := event_data;
                        log_ptr <= log_ptr + 1;
                    else
                        log_overflow <= '1';
                    end if;
                end if;

                -- cycle_done: 배치 완료 카운팅
                if cycle_done = '1' then
                    if cycle_cnt = CYCLES_PER_BATCH - 1 then
                        -- ── 배치 완료: 헤더 확정 + is_full 설정 ──
                        -- 헤더 포맷 (32bit):
                        --   [31:30] = "00"         (예약)
                        --   [29]    = log_overflow  (로그 오버플로우 플래그)
                        --   [28:13] = batch_counter (16bit, 배치 번호)
                        --   [12]    = "1"           (유효 마커)
                        --   [11:0]  = log_ptr-1     (12bit, 유효 항목 수)
                        log_ram(0) := "00"
                                    & log_overflow
                                    & std_logic_vector(batch_counter)
                                    & "1"
                                    & std_logic_vector(resize(log_ptr - 1, 12));
                        is_full      <= '1';
                        batch_counter <= batch_counter + 1;
                        cycle_cnt    <= 0;  -- 다음 배치 준비 (is_full=0 될 때까지 무의미)
                    else
                        cycle_cnt <= cycle_cnt + 1;
                    end if;
                end if;

            end if;
            -- is_full='1'이면 event_we, cycle_done 모두 무시 (else 없음)

        end if;
    end process;

    -- ============================================================
    -- 🔵 125MHz: IPbus 읽기 + Toggle 생성
    -- ============================================================
    process(clk)
    begin
        if rising_edge(clk) then
            -- 1-cycle 읽기 지연 (BRAM registered output)
            q_log <= log_ram(to_integer(unsigned(ipb_in_log.ipb_addr(11 downto 0))));

            if rst = '1' then
                ack_log           <= '0';
                reset_toggle_clk  <= '0';
            else
                -- IPbus 1 wait-state ACK
                ack_log <= ipb_in_log.ipb_strobe and not ack_log;

                -- 마지막 주소(0xFFF) 읽을 때 Toggle 반전
                -- ack_log='0' 조건으로 strobe 첫 사이클에만 반응 (글리치 방지)
                if ipb_in_log.ipb_strobe = '1'
                        and ack_log = '0'
                        and ipb_in_log.ipb_addr(11 downto 0) = x"FFF" then
                    reset_toggle_clk <= not reset_toggle_clk;
                end if;
            end if;
        end if;
    end process;

    ipb_out_log.ipb_rdata <= q_log;
    ipb_out_log.ipb_ack   <= ack_log;
    ipb_out_log.ipb_err   <= '0';

end rtl;
library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity etroc_rx_bridge is
    port (
        clk        : in  std_logic;  
        dpram_full : in  std_logic;
        dpram_we   : out std_logic;
        dpram_d    : out std_logic_vector(31 downto 0);

        clk8       : in  std_logic;  
        d_left     : in  std_logic_vector(39 downto 0);
        d_right    : in  std_logic_vector(39 downto 0);
        type_left  : in  std_logic_vector(1 downto 0);
        type_right : in  std_logic_vector(1 downto 0);

        debug_type_left  : out std_logic_vector(1 downto 0);
        debug_type_right : out std_logic_vector(1 downto 0)
    );
end etroc_rx_bridge;

architecture rtl of etroc_rx_bridge is
    component vio_0
        port ( clk : in std_logic; probe_out0 : out std_logic_vector(0 downto 0) );
    end component;

    component async_fifo_84b is
      port (
        rst, wr_clk, rd_clk : in std_logic;
        din    : in std_logic_vector(83 downto 0);
        wr_en, rd_en : in std_logic;
        dout   : out std_logic_vector(83 downto 0);
        full, empty  : out std_logic
      );
    end component;

    signal rst_vio_vec  : std_logic_vector(0 downto 0);
    signal rst_internal : std_logic;

    signal fifo_din   : std_logic_vector(83 downto 0);
    signal fifo_wr_en, fifo_rd_en, fifo_empty, fifo_full : std_logic;
    signal fifo_dout  : std_logic_vector(83 downto 0);

    signal wr_cnt      : integer range 0 to 5 := 0;
    signal cap_data_l, cap_data_r : std_logic_vector(39 downto 0) := (others => '0');
    signal cap_type_l, cap_type_r : std_logic_vector(1 downto 0)  := (others => '0');

begin
    debug_type_left  <= type_left;
    debug_type_right <= type_right;

    vio_inst : vio_0 port map ( clk => clk, probe_out0 => rst_vio_vec );
    rst_internal <= rst_vio_vec(0);

    process(clk8)
    begin
        if rising_edge(clk8) then
            if rst_internal = '1' then
                fifo_wr_en <= '0';
                fifo_din   <= (others => '0');
            else
                if fifo_full = '0' then
                    fifo_din   <= type_left & type_right & d_left & d_right;
                    fifo_wr_en <= '1';
                else
                    fifo_wr_en <= '0';
                end if;
            end if;
        end if;
    end process;

    fifo_inst : async_fifo_84b port map (
        rst => rst_internal, wr_clk => clk8, rd_clk => clk,
        din => fifo_din, wr_en => fifo_wr_en, rd_en => fifo_rd_en,
        dout => fifo_dout, full => fifo_full, empty => fifo_empty
    );

    process(clk)
    begin
        if rising_edge(clk) then
            if rst_internal = '1' then
                wr_cnt <= 0; dpram_we <= '0'; fifo_rd_en <= '0';
            else
                dpram_we <= '0'; fifo_rd_en <= '0'; 

                case wr_cnt is
                    when 0 =>
                        if fifo_empty = '0' and dpram_full = '0' then
                            fifo_rd_en <= '1'; wr_cnt <= 1;   
                        end if;
                    when 1 =>
                        cap_type_l <= fifo_dout(83 downto 82);
                        cap_type_r <= fifo_dout(81 downto 80);
                        cap_data_l <= fifo_dout(79 downto 40);
                        cap_data_r <= fifo_dout(39 downto  0);
                        wr_cnt     <= 2;
                    when 2 =>
                        if (cap_type_l /= "11") or (cap_type_r /= "11") then
                            -- Word 1 (Marker: 00) - Right LSBs (29 bits)
                            dpram_d  <= "1" & "00" & cap_data_r(28 downto 0);
                            dpram_we <= '1'; wr_cnt <= 3;
                        else
                            wr_cnt <= 0;
                        end if;
                    when 3 =>
                        -- Word 2 (Marker: 01) - Right MSBs (11 bits) + Type (2 bits)
                        dpram_d  <= "1" & "01" & "0000000000000000" & cap_type_r & cap_data_r(39 downto 29);
                        dpram_we <= '1'; wr_cnt <= 4;
                    when 4 =>
                        -- Word 3 (Marker: 10) - Left LSBs (29 bits)
                        dpram_d  <= "1" & "10" & cap_data_l(28 downto 0);
                        dpram_we <= '1'; wr_cnt <= 5;
                    when 5 =>
                        -- Word 4 (Marker: 11) - Left MSBs (11 bits) + Type (2 bits)
                        dpram_d  <= "1" & "11" & "0000000000000000" & cap_type_l & cap_data_l(39 downto 29);
                        dpram_we <= '1'; wr_cnt <= 0; 
                end case;
            end if;
        end if;
    end process;
end rtl;