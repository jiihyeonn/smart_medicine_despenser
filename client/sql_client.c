/*
 * sql_client.c - DB 게이트웨이
 *
 * GETDB 포맷:
 *   ① UserTable  : [SQL]GETDB@USERID@{ID}@{컬럼}
 *   ② Inventory  : [SQL]GETDB@SLOTNUM@{SlotNum}@{컬럼}
 *   ③ MedicineRule: [SQL]GETDB@SYMPTOM@{증상}
 *   ④ Inventory  : [SQL]GETDB@MEDCODE@{MedCode}
 *
 * 빌드: gcc -o sql_client sql_client.c -lpthread -lmysqlclient
 * 실행: ./sql_client <서버IP> <port> SQL
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <mysql/mysql.h>

#define BUF_SIZE  512
#define NAME_SIZE 20
#define ARR_CNT   10

void* send_msg(void* arg);
void* recv_msg(void* arg);
void  error_handling(char* msg);

char name[NAME_SIZE] = "[Default]";
char msg[BUF_SIZE];

int main(int argc, char* argv[])
{
    int sock;
    struct sockaddr_in serv_addr;
    pthread_t snd_thread, rcv_thread;
    void* thread_return;

    if (argc != 4) {
        printf("Usage : %s <IP> <port> <name>\n", argv[0]);
        exit(1);
    }
    sprintf(name, "%s", argv[3]);

    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock == -1) error_handling("socket() error");

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
    serv_addr.sin_port        = htons(atoi(argv[2]));

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1)
        error_handling("connect() error");

    sprintf(msg, "[%s:PASSWD]", name);
    write(sock, msg, strlen(msg));

    pthread_create(&rcv_thread, NULL, recv_msg, (void*)&sock);
    pthread_create(&snd_thread, NULL, send_msg, (void*)&sock);

    pthread_join(snd_thread, &thread_return);
    pthread_join(rcv_thread, &thread_return);

    if (sock != -1) close(sock);
    return 0;
}

void* send_msg(void* arg)
{
    int* sock = (int*)arg;
    int ret;
    fd_set initset, newset;
    struct timeval tv;
    char name_msg[NAME_SIZE + BUF_SIZE + 2];

    FD_ZERO(&initset);
    FD_SET(STDIN_FILENO, &initset);
    fputs("Input a message! [ID]msg (Default ID:ALLMSG)\n", stdout);

    while (1) {
        memset(msg, 0, sizeof(msg));
        name_msg[0] = '\0';
        tv.tv_sec = 1; tv.tv_usec = 0;
        newset = initset;
        ret = select(STDIN_FILENO + 1, &newset, NULL, NULL, &tv);
        if (FD_ISSET(STDIN_FILENO, &newset)) {
            fgets(msg, BUF_SIZE, stdin);
            if (!strncmp(msg, "quit\n", 5)) { *sock = -1; return NULL; }
            else if (msg[0] != '[') { strcat(name_msg, "[ALLMSG]"); strcat(name_msg, msg); }
            else strcpy(name_msg, msg);
            if (write(*sock, name_msg, strlen(name_msg)) <= 0) { *sock = -1; return NULL; }
        }
        if (ret == 0 && *sock == -1) return NULL;
    }
}

void* recv_msg(void* arg)
{
    MYSQL*     conn;
    MYSQL_ROW  sqlrow;
    MYSQL_RES* result;
    int   res;
    char  sql_cmd[512] = {0};
    char* host   = "localhost";
    char* user   = "iot";
    char* pass   = "pwiot";
    char* dbname = "SmartDispenser";

    int*  sock = (int*)arg;
    int   i;
    char* pToken;
    char* pArray[ARR_CNT];
    char  name_msg[NAME_SIZE + BUF_SIZE + 1];
    int   str_len;
    int   illu;
    float temp, humi;

    conn = mysql_init(NULL);
    puts("MYSQL startup");
    if (!(mysql_real_connect(conn, host, user, pass, dbname, 0, NULL, CLIENT_FOUND_ROWS))) {
        fprintf(stderr, "ERROR : %s[%d]\n", mysql_error(conn), mysql_errno(conn));
        exit(1);
    }
    printf("Connection Successful!\n\n");

    while (1) {
        memset(name_msg, 0x0, sizeof(name_msg));
        memset(pArray,   0x0, sizeof(pArray));

        str_len = read(*sock, name_msg, NAME_SIZE + BUF_SIZE);
        if (str_len <= 0) { *sock = -1; break; }

        fputs(name_msg, stdout);
        name_msg[str_len] = '\0';
        name_msg[strcspn(name_msg, "\n")] = '\0';
        name_msg[strcspn(name_msg, "\r")] = '\0';

        pToken = strtok(name_msg, "[:@]");
        i = 0;
        while (pToken != NULL) {
            pArray[i] = pToken;
            if (++i >= ARR_CNT) break;
            pToken = strtok(NULL, "[:@]");
        }
        if (i < 2 || pArray[1] == NULL) continue;

        /* ════════════════════════════════════════════════════════
         * [기능 1] GETDB
         * ════════════════════════════════════════════════════════ */
        if (!strcmp(pArray[1], "GETDB"))
        {
            if (i < 4) {
                sprintf(sql_cmd, "[%s]GETDB@ERROR@BAD_PACKET\n", pArray[0]);
                write(*sock, sql_cmd, strlen(sql_cmd));
                continue;
            }

            /* ① UserTable 조회
             * 수신: [DOC]GETDB@USERID@010303@MedicineCode
             * 반환: [DOC]GETDB@010303@MedicineCode@값 */
            if (!strcmp(pArray[2], "USERID"))
            {
                char target_col[50];
                strcpy(target_col, (i >= 5) ? pArray[4] : "UserName");

                printf("[GETDB] UserTable → ID:%s, 컬럼:%s\n", pArray[3], target_col);
                sprintf(sql_cmd,
                    "SELECT %s FROM UserTable WHERE UserID = '%s'",
                    target_col, pArray[3]);

                if (mysql_query(conn, sql_cmd)) {
                    fprintf(stderr, "[GETDB] SQL 오류: %s\n", mysql_error(conn));
                    sprintf(sql_cmd, "[%s]GETDB@%s@FAIL\n", pArray[0], pArray[3]);
                    write(*sock, sql_cmd, strlen(sql_cmd));
                    continue;
                }
                result = mysql_store_result(conn);
                sqlrow = mysql_fetch_row(result);
                if (sqlrow && sqlrow[0])
                    sprintf(sql_cmd, "[%s]GETDB@%s@%s@%s\n",
                            pArray[0], pArray[3], target_col, sqlrow[0]);
                else
                    sprintf(sql_cmd, "[%s]GETDB@%s@NOT_FOUND\n",
                            pArray[0], pArray[3]);
                write(*sock, sql_cmd, strlen(sql_cmd));
                mysql_free_result(result);
            }

            /* ② Inventory 조회 (SlotNum 기준)
             * 수신: [STM]GETDB@SLOTNUM@1@Stock
             * 반환: [STM]GETDB@1@Stock@50 */
            else if (!strcmp(pArray[2], "SLOTNUM"))
            {
                char target_col[50];
                strcpy(target_col, (i >= 5) ? pArray[4] : "Stock");

                printf("[GETDB] Inventory → SlotNum:%s, 컬럼:%s\n", pArray[3], target_col);
                sprintf(sql_cmd,
                    "SELECT %s FROM Inventory WHERE SlotNum = %s",
                    target_col, pArray[3]);

                if (mysql_query(conn, sql_cmd)) {
                    fprintf(stderr, "[GETDB] SQL 오류: %s\n", mysql_error(conn));
                    sprintf(sql_cmd, "[%s]GETDB@%s@FAIL\n", pArray[0], pArray[3]);
                    write(*sock, sql_cmd, strlen(sql_cmd));
                    continue;
                }
                result = mysql_store_result(conn);
                sqlrow = mysql_fetch_row(result);
                if (sqlrow && sqlrow[0])
                    sprintf(sql_cmd, "[%s]GETDB@%s@%s@%s\n",
                            pArray[0], pArray[3], target_col, sqlrow[0]);
                else
                    sprintf(sql_cmd, "[%s]GETDB@%s@NOT_FOUND\n",
                            pArray[0], pArray[3]);
                write(*sock, sql_cmd, strlen(sql_cmd));
                mysql_free_result(result);
            }

            /* ③ MedicineRule 조회 (증상 기준)
             * 수신: [DOC]GETDB@SYMPTOM@Headache
             * 반환: [DOC]GETDB@Headache@Tylenol@1@13@2@1@19@NONE
             *       증상@MedName@MedCode@MinAge@BaseCount@TeenCount@AgeLimit@AllergyTag */
            else if (!strcmp(pArray[2], "SYMPTOM"))
            {
                printf("[GETDB] MedicineRule → 증상:%s\n", pArray[3]);
                sprintf(sql_cmd,
                    "SELECT MedName, MedCode, MinAge, BaseCount, "
                    "TeenCount, AgeLimit, AllergyTag "
                    "FROM MedicineRule WHERE SymptomName = '%s'",
                    pArray[3]);

                if (mysql_query(conn, sql_cmd)) {
                    fprintf(stderr, "[GETDB] SQL 오류: %s\n", mysql_error(conn));
                    sprintf(sql_cmd, "[%s]GETDB@%s@NOT_FOUND\n", pArray[0], pArray[3]);
                    write(*sock, sql_cmd, strlen(sql_cmd));
                    continue;
                }
                result = mysql_store_result(conn);
                sqlrow = mysql_fetch_row(result);
                if (sqlrow)
                    sprintf(sql_cmd, "[%s]GETDB@%s@%s@%s@%s@%s@%s@%s@%s\n",
                            pArray[0], pArray[3],
                            sqlrow[0],  // MedName
                            sqlrow[1],  // MedCode
                            sqlrow[2],  // MinAge
                            sqlrow[3],  // BaseCount
                            sqlrow[4],  // TeenCount
                            sqlrow[5],  // AgeLimit
                            sqlrow[6]); // AllergyTag
                else
                    sprintf(sql_cmd, "[%s]GETDB@%s@NOT_FOUND\n", pArray[0], pArray[3]);
                write(*sock, sql_cmd, strlen(sql_cmd));
                mysql_free_result(result);
            }

            /* ④ Inventory 조회 (MedCode 기준)
             * 수신: [DOC]GETDB@MEDCODE@1
             * 반환: [DOC]GETDB@MEDCODE@Tylenol@1@1@100
             *       MEDCODE@MedName@SlotNum@ServoChannel@Stock */
            else if (!strcmp(pArray[2], "MEDCODE"))
            {
                printf("[GETDB] Inventory → MedCode:%s\n", pArray[3]);
                sprintf(sql_cmd,
                    "SELECT MedName, SlotNum, ServoChannel, Stock "
                    "FROM Inventory WHERE MedicineCode = %s",
                    pArray[3]);

                if (mysql_query(conn, sql_cmd)) {
                    fprintf(stderr, "[GETDB] SQL 오류: %s\n", mysql_error(conn));
                    sprintf(sql_cmd, "[%s]GETDB@MEDCODE@NOT_FOUND\n", pArray[0]);
                    write(*sock, sql_cmd, strlen(sql_cmd));
                    continue;
                }
                result = mysql_store_result(conn);
                sqlrow = mysql_fetch_row(result);
                if (sqlrow)
                    sprintf(sql_cmd, "[%s]GETDB@MEDCODE@%s@%s@%s@%s\n",
                            pArray[0],
                            sqlrow[0],  // MedName
                            sqlrow[1],  // SlotNum
                            sqlrow[2],  // ServoChannel
                            sqlrow[3]); // Stock
                else
                    sprintf(sql_cmd, "[%s]GETDB@MEDCODE@NOT_FOUND\n", pArray[0]);
                write(*sock, sql_cmd, strlen(sql_cmd));
                mysql_free_result(result);
            }

            else {
                printf("[GETDB] 알 수 없는 키타입: %s\n", pArray[2]);
                sprintf(sql_cmd, "[%s]GETDB@ERROR@UNKNOWN_KEY\n", pArray[0]);
                write(*sock, sql_cmd, strlen(sql_cmd));
            }
        }

        /* ════════════════════════════════════════════════════════
         * [기능 2] SETDB : UserTable 신규 등록/수정
         * 수신: [LOG]SETDB@010303@CMJ@26@NONE
         * 반환: [LOG]SETDB@010303@OK / FAIL
         * ════════════════════════════════════════════════════════ */
        else if (!strcmp(pArray[1], "SETDB"))
        {
            if (i != 6) {
                printf("[SETDB] 인자 개수 오류 (%d개)\n", i);
                sprintf(sql_cmd, "[%s]SETDB@ERROR@BAD_PACKET\n", pArray[0]);
                write(*sock, sql_cmd, strlen(sql_cmd));
                continue;
            }
            if (!pArray[2] || !pArray[3] ||
                strlen(pArray[2]) == 0 || strlen(pArray[3]) == 0) {
                printf("[SETDB] 누락 데이터\n");
                sprintf(sql_cmd, "[%s]SETDB@ERROR@EMPTY_DATA\n", pArray[0]);
                write(*sock, sql_cmd, strlen(sql_cmd));
                continue;
            }

            printf("[SETDB] 등록/수정 → ID:%s, 이름:%s\n", pArray[2], pArray[3]);
            sprintf(sql_cmd,
                "INSERT INTO UserTable "
                "(UserID, UserName, UserAge, Allergy, MedicineCode, CumMedicine) "
                "VALUES ('%s', '%s', %s, '%s', 0, 0) "
                "ON DUPLICATE KEY UPDATE "
                "UserName='%s', UserAge=%s, Allergy='%s'",
                pArray[2], pArray[3], pArray[4], pArray[5],
                pArray[3], pArray[4], pArray[5]);

            res = mysql_query(conn, sql_cmd);
            if (!res) {
                unsigned long affected = (unsigned long)mysql_affected_rows(conn);
                printf("[SETDB] %s → ID:%s\n",
                       affected == 1 ? "신규 등록" : "정보 수정", pArray[2]);
                sprintf(sql_cmd, "[%s]SETDB@%s@OK\n", pArray[0], pArray[2]);
            } else {
                fprintf(stderr, "[SETDB] SQL 에러: %s\n", mysql_error(conn));
                sprintf(sql_cmd, "[%s]SETDB@%s@FAIL\n", pArray[0], pArray[2]);
            }
            write(*sock, sql_cmd, strlen(sql_cmd));
        }

        /* ════════════════════════════════════════════════════════
         * [기능 3] UPDATEDB : 단일 컬럼 수정
         *
         * UserTable:
         *   수신: [DOC]UPDATEDB@010303@MedicineCode@201
         *         [DOC]UPDATEDB@010303@CumMedicine@2   (누적)
         *         [DOC]UPDATEDB@010303@CumMedicine@0   (초기화)
         *
         * Inventory:
         *   수신: [DOC]UPDATEDB@1@Stock@3   (재고 감소)
         *
         * 반환: [DOC]UPDATEDB@010303@OK / NOT_FOUND / FAIL
         * ════════════════════════════════════════════════════════ */
        else if (!strcmp(pArray[1], "UPDATEDB"))
        {
            if (i != 5) {
                printf("[UPDATEDB] 인자 개수 오류 (%d개)\n", i);
                sprintf(sql_cmd, "[%s]UPDATEDB@ERROR@BAD_PACKET\n", pArray[0]);
                write(*sock, sql_cmd, strlen(sql_cmd));
                continue;
            }

            /* Inventory.Stock 감소 */
            if (!strcmp(pArray[3], "Stock")) {
                printf("[UPDATEDB] Inventory Stock 감소 → SlotNum:%s, 수량:%s\n",
                       pArray[2], pArray[4]);
                sprintf(sql_cmd,
                    "UPDATE Inventory SET Stock = Stock - %s "
                    "WHERE SlotNum = %s AND Stock >= %s",
                    pArray[4], pArray[2], pArray[4]);
            }
            /* UserTable.CumMedicine 누적/초기화 */
            else if (!strcmp(pArray[3], "CumMedicine")) {
                if (atoi(pArray[4]) == 0)
                    sprintf(sql_cmd,
                        "UPDATE UserTable SET CumMedicine = 0 "
                        "WHERE UserID = '%s'", pArray[2]);
                else
                    sprintf(sql_cmd,
                        "UPDATE UserTable SET CumMedicine = CumMedicine + %s "
                        "WHERE UserID = '%s'", pArray[4], pArray[2]);
            }
            /* UserTable 문자열 컬럼 */
            else if (!strcmp(pArray[3], "UserName") ||
                     !strcmp(pArray[3], "Allergy")) {
                sprintf(sql_cmd,
                    "UPDATE UserTable SET %s = '%s' WHERE UserID = '%s'",
                    pArray[3], pArray[4], pArray[2]);
            }
            /* UserTable 숫자 컬럼 (MedicineCode, UserAge 등) */
            else {
                sprintf(sql_cmd,
                    "UPDATE UserTable SET %s = %s WHERE UserID = '%s'",
                    pArray[3], pArray[4], pArray[2]);
            }

            printf("[UPDATEDB] 쿼리: %s\n", sql_cmd);
            res = mysql_query(conn, sql_cmd);
            if (!res) {
                unsigned long affected = (unsigned long)mysql_affected_rows(conn);
                if (affected > 0) {
                    printf("[UPDATEDB] 성공 → ID:%s, %s 변경\n", pArray[2], pArray[3]);
                    sprintf(sql_cmd, "[%s]UPDATEDB@%s@OK\n", pArray[0], pArray[2]);
                } else {
                    printf("[UPDATEDB] 실패 → 없는 ID 또는 재고 부족\n");
                    sprintf(sql_cmd, "[%s]UPDATEDB@%s@NOT_FOUND\n", pArray[0], pArray[2]);
                }
            } else {
                fprintf(stderr, "[UPDATEDB] SQL 에러: %s\n", mysql_error(conn));
                sprintf(sql_cmd, "[%s]UPDATEDB@%s@FAIL\n", pArray[0], pArray[2]);
            }
            write(*sock, sql_cmd, strlen(sql_cmd));
        }

        /* ════════════════════════════════════════════════════════
         * [기능 4] SENSOR : 센서 데이터 INSERT
         * ════════════════════════════════════════════════════════ */
        else if (!strcmp(pArray[1], "SENSOR") && i == 5)
        {
            illu = atoi(pArray[2]);
            temp = atof(pArray[3]);
            humi = atof(pArray[4]);
            sprintf(sql_cmd,
                "INSERT INTO sensor(name, date, time, illu, temp, humi) "
                "VALUES('%s', now(), now(), %d, %f, %f)",
                pArray[0], illu, temp, humi);
            res = mysql_query(conn, sql_cmd);
            if (!res)
                printf("[SENSOR] 저장 완료\n");
            else
                fprintf(stderr, "[SENSOR] 오류: %s\n", mysql_error(conn));
        }

        /* ════════════════════════════════════════════════════════
         * [기능 5] CHECK_LIMIT : 수령 통계 + 나이 조회
         * 수신: [DOC]CHECK_LIMIT@010303
         * 반환: [DOC]CHECK_LIMIT_RES@010303@24h횟수@누적수@나이
         * ════════════════════════════════════════════════════════ */
        else if (!strcmp(pArray[1], "CHECK_LIMIT") && i >= 3)
        {
            printf("[CHECK_LIMIT] ID:%s 조회\n", pArray[2]);
            sprintf(sql_cmd,
                "SELECT "
                "(SELECT COUNT(*) FROM DispenseLog "
                " WHERE UserID='%s' AND DispenseTime >= NOW() - INTERVAL 24 HOUR), "
                "CumMedicine, UserAge "
                "FROM UserTable WHERE UserID='%s'",
                pArray[2], pArray[2]);

            if (mysql_query(conn, sql_cmd) == 0) {
                result = mysql_store_result(conn);
                sqlrow = mysql_fetch_row(result);
                if (sqlrow && sqlrow[0] && sqlrow[1] && sqlrow[2]) {
                    printf(" → 24h:%s회 / 누적:%s개 / 나이:%s세\n",
                           sqlrow[0], sqlrow[1], sqlrow[2]);
                    sprintf(sql_cmd, "[%s]CHECK_LIMIT_RES@%s@%s@%s@%s\n",
                            pArray[0], pArray[2],
                            sqlrow[0], sqlrow[1], sqlrow[2]);
                } else {
                    printf(" → 미등록 유저: %s\n", pArray[2]);
                    sprintf(sql_cmd, "[%s]CHECK_LIMIT_RES@%s@ERROR@ERROR\n",
                            pArray[0], pArray[2]);
                }
                mysql_free_result(result);
            } else {
                fprintf(stderr, "[CHECK_LIMIT] SQL 오류: %s\n", mysql_error(conn));
                sprintf(sql_cmd, "[%s]CHECK_LIMIT_RES@%s@ERROR@ERROR\n",
                        pArray[0], pArray[2]);
            }
            write(*sock, sql_cmd, strlen(sql_cmd));
        }

        /* ════════════════════════════════════════════════════════
         * [기능 6] DISPENSE_LOG : 배출 기록 INSERT
         * 수신: [DOC]DISPENSE_LOG@010303@201
         * 반환: [DOC]DISPENSE_LOG@010303@OK / FAIL
         * ════════════════════════════════════════════════════════ */
        else if (!strcmp(pArray[1], "DISPENSE_LOG") && i >= 4)
        {
            printf("[DISPENSE_LOG] ID:%s, 코드:%s 기록\n", pArray[2], pArray[3]);
            sprintf(sql_cmd,
                "INSERT INTO DispenseLog (UserID, MedicineCode, DispenseTime) "
                "VALUES ('%s', %s, NOW())",
                pArray[2], pArray[3]);

            if (mysql_query(conn, sql_cmd) == 0) {
                printf(" → 기록 완료\n");
                sprintf(sql_cmd, "[%s]DISPENSE_LOG@%s@OK\n", pArray[0], pArray[2]);
            } else {
                fprintf(stderr, "[DISPENSE_LOG] SQL 오류: %s\n", mysql_error(conn));
                sprintf(sql_cmd, "[%s]DISPENSE_LOG@%s@FAIL\n", pArray[0], pArray[2]);
            }
            write(*sock, sql_cmd, strlen(sql_cmd));
        }

    } /* end while */

    mysql_close(conn);
    return NULL;
}

void error_handling(char* msg)
{
    fputs(msg, stderr);
    fputc('\n', stderr);
    exit(1);
}