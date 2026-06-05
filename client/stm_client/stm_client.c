/*
 * stm_client.c - BT ↔ TCP 단순 중계 + ENV 센서 데이터 DB 저장
 *
 * 역할:
 *   1) BT로 STM32 메시지 수신 → TCP 서버로 전달
 *   2) TCP 서버 메시지 → BT로 STM32에 전달
 *   3) [ENV@TEMP:25.3@HUM:45] 수신 시 iotdb.sensor 테이블에 저장
 *      (세션 창에 온도/습도 출력 안 함)
 *
 * 빌드: gcc -o stm_client stm_client.c -lpthread -lmysqlclient
 * 실행: ./stm_client <서버IP> <port> STM
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <mysql/mysql.h>

#define BUF_SIZE  512
#define BT_PORT   "/dev/rfcomm0"
#define BT_BAUD   B9600

int tcp_sock = -1;
int bt_fd    = -1;

/* ??????????????????????????????????????????????
 * DB 연결 및 센서 데이터 저장
 * ?????????????????????????????????????????????? */
void save_sensor_to_db(float temp, float humi)
{
    MYSQL* conn = mysql_init(NULL);
    if (!conn) return;

    if (!mysql_real_connect(conn, "127.0.0.1", "iot", "pwiot",
                            "iotdb", 0, NULL, 0)) {
        fprintf(stderr, "[DB] Connection Failed: %s\n", mysql_error(conn));
        mysql_close(conn);
        return;
    }

    char sql_cmd[256] = {0};
    sprintf(sql_cmd,
        "INSERT INTO sensor(name, date, time, illu, temp, humi) "
        "VALUES('STM', NOW(), NOW(), 0, %f, %f)",
        temp, humi);

    if (mysql_query(conn, sql_cmd) == 0)
        printf("[DB] Sensor Saved (temp:%.1f humi:%.1f)\n", temp, humi);
    else
        fprintf(stderr, "[DB] Save failed: %s\n", mysql_error(conn));

    mysql_close(conn);
}

/* ??????????????????????????????????????????????
 * ENV 메시지 파싱
 * [ENV@TEMP:25.3@HUM:45]
 * ?????????????????????????????????????????????? */
void parse_env_and_save(const char *msg)
{
    float temp = 0, humi = 0;
    char *temp_ptr = strstr(msg, "TEMP:");
    char *humi_ptr = strstr(msg, "HUM:");

    if (temp_ptr) temp = atof(temp_ptr + 5);
    if (humi_ptr) humi = atof(humi_ptr + 4);

    if (temp > 0 || humi > 0)
        save_sensor_to_db(temp, humi);
}

/* ??????????????????????????????????????????????
 * 서버 → STM32 방향 중계
 * ?????????????????????????????????????????????? */
void *tcp_to_bt(void *arg)
{
    char buf[BUF_SIZE];
    int  n;
    while (1) {
        memset(buf, 0, sizeof(buf));
        n = read(tcp_sock, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        printf("[Server→STM32] %s", buf);
        write(bt_fd, buf, n);
    }
    return NULL;
}

/* ??????????????????????????????????????????????
 * STM32 → 서버 방향 중계
 * ENV 메시지는 DB 저장 후 서버 전달 안 함
 * ?????????????????????????????????????????????? */
void *bt_to_tcp(void *arg)
{
    char buf[BUF_SIZE];
    char line[BUF_SIZE];
    int  idx = 0;
    int  n;

    while (1) {
        memset(buf, 0, sizeof(buf));
        n = read(bt_fd, buf, sizeof(buf) - 1);
        if (n <= 0) continue;

        /* 줄 단위 처리 */
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                if (idx > 0) {
                    line[idx] = '\0';

                    /* ENV 메시지 처리 → DB 저장, 서버 전달 안 함 */
                    if (strstr(line, "ENV@TEMP") != NULL) {
                        parse_env_and_save(line);
                        /* 세션에 온도/습도 출력 안 함 */
                    }
                    else {
                        /* 일반 메시지 → 서버로 전달 */
                        printf("[STM32→ Server] %s\n", line);
                        line[idx] = '\n';
                        write(tcp_sock, line, idx + 1);
                    }
                    idx = 0;
                }
            } else {
                if (idx < BUF_SIZE - 2)
                    line[idx++] = buf[i];
            }
        }
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    struct sockaddr_in serv_addr;
    pthread_t t1, t2;
    char msg[64];
    int  n;

    if (argc != 4) {
        printf("Usage: %s <IP> <port> STM\n", argv[0]);
        exit(1);
    }

    /* ── BT 시리얼 열기 ── */
    bt_fd = open(BT_PORT, O_RDWR | O_NOCTTY);
    if (bt_fd < 0) {
        perror("BT 포트 열기 실패");
        printf("먼저 실행: sudo rfcomm bind /dev/rfcomm0 XX:XX:XX\n");
        exit(1);
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    tcgetattr(bt_fd, &tty);
    cfsetispeed(&tty, BT_BAUD);
    cfsetospeed(&tty, BT_BAUD);
    tty.c_cflag = CS8 | CREAD | CLOCAL;
    tty.c_iflag = IGNPAR;
    tty.c_oflag = 0;
    tty.c_lflag = 0;
    tty.c_cc[VMIN]  = 1;
    tty.c_cc[VTIME] = 0;
    tcsetattr(bt_fd, TCSANOW, &tty);
    printf("[STM] BT connected: %s\n", BT_PORT);

    /* ── TCP 서버 연결 ── */
    tcp_sock = socket(PF_INET, SOCK_STREAM, 0);
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
    serv_addr.sin_port        = htons(atoi(argv[2]));

    if (connect(tcp_sock, (struct sockaddr*)&serv_addr,
                sizeof(serv_addr)) < 0) {
        perror("Server Connection Failed");
        exit(1);
    }

    /* ── STM으로 로그인 ── */
    sprintf(msg, "[%s:PASSWD]\n", argv[3]);
    write(tcp_sock, msg, strlen(msg));

    n = read(tcp_sock, msg, sizeof(msg) - 1);
    msg[n] = '\0';
    printf("[STM] Server response: %s", msg);

    if (!strstr(msg, "New connected")) {
        printf("[STM] 로그인 실패\n");
        exit(1);
    }
    printf("[STM] Login Success → recording start\n");

    /* ── 양방향 중계 ── */
    pthread_create(&t1, NULL, tcp_to_bt, NULL);
    pthread_create(&t2, NULL, bt_to_tcp, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    close(tcp_sock);
    close(bt_fd);
    return 0;
}