/*
 * doc_client.c
 * ─────────────────────────────────────────────────────────────
 * [흐름 1 - 예약 픽업] STM -> DOC
 *   STM  -> [DOC]PICKUP@UserID
 *   DOC  -> [SQL]GETDB@USERID@UserID@MedicineCode
 *   SQL  -> [DOC]GETDB@UserID@MedicineCode@Code
 *     code==0/NOT_FOUND -> [STM]PICKUP@FAIL_EMPTY@0
 *     code>0 ->
 *       DOC -> [SQL]GETDB@MEDCODE@{MedType}       (약이름+ServoChannel 조회)
 *       SQL -> [DOC]GETDB@MEDCODE@MedName@SlotNum@ServoChannel@Stock
 *       재고 부족 -> [STM]PICKUP@FAIL_STOCK@0
 *       DOC -> [STM]PICKUP@MedName@Count@ServoChannel (배출 승인, DB 변경 없음)
 *       STM -> [DOC]PICKUP_DONE@UserID@Med@Count   (배출 완료)
 *         DOC -> [SQL]DISPENSE_LOG
 *         DOC -> [SQL]UPDATEDB CumMedicine +
 *         DOC -> [SQL]UPDATEDB MedicineCode=0
 *         DOC -> [SQL]UPDATEDB Stock -              (재고 감소)
 *       STM -> [DOC]PICKUP_FAIL                    (배출 실패 → DB 유지)
 *
 * [흐름 2 - 증상 예약] AND/STM -> DOC
 *   AND/STM -> [DOC]SYMPTOM@UserID@SymptomName
 *   DOC -> [SQL]GETDB@SYMPTOM@SymptomName          (MedicineRule 조회)
 *   SQL -> [DOC]GETDB@증상@MedName@MedCode@MinAge@BaseCount@TeenCount@AgeLimit@AllergyTag
 *   DOC -> [SQL]GETDB@USERID@UserID@Allergy        (알러지 조회)
 *   SQL -> [DOC]GETDB@UserID@Allergy@값
 *     알러지 일치 → FAIL_ALLERGY
 *   DOC -> [SQL]CHECK_LIMIT@UserID
 *   SQL -> [DOC]CHECK_LIMIT_RES@UserID@24h@Total@Age
 *     ① 24h >= 2          → FAIL_24H_LIMIT
 *     ② Total >= 30       → FAIL_TOTAL_LIMIT
 *     ③ decide_count() = 0 → FAIL_OVERDOSE or FAIL_AGE_LIMIT
 *   DOC -> [SQL]GETDB@USERID@UserID@MedicineCode   (기존 예약 확인)
 *   DOC -> [SQL]UPDATEDB@UserID@MedicineCode@Code  (예약 확정)
 *   DOC -> [요청자]PRESCRIPTION@MedName@Amount
 *
 * MedicineCode 체계: 앞자리(약종류)*100 + 뒷자리(개수)
 *   ex) 102 = Tylenol 2알, 301 = Buscopan 1알
 *
 * 빌드: gcc -o doc_client doc_client.c -lpthread
 * 실행: ./doc_client <서버IP> <port> DOC
 * ─────────────────────────────────────────────────────────────
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>

#define BUF_SIZE  256
#define NAME_SIZE 20
#define ARR_CNT   10

void* send_msg(void* arg);
void* recv_msg(void* arg);
void  error_handling(char* msg);

char name[NAME_SIZE] = "DOC";
char msg[BUF_SIZE];

typedef enum {
    STEP_NONE = 0,
    /* 증상 처방(예약) 흐름 */
    STEP_WAIT_MED_INFO,    /* MedicineRule 조회 대기 */
    STEP_WAIT_ALLERGY,     /* 유저 알러지 조회 대기 */
    STEP_WAIT_LIMIT,       /* CHECK_LIMIT_RES 대기 */
    STEP_WAIT_RESV_CHECK,  /* GETDB(MedicineCode) 대기 - 기존 예약 확인 */
    STEP_WAIT_CODE,        /* UPDATEDB(MedicineCode) 대기 - 예약 확정 */
    /* 예약 픽업 흐름 */
    STEP_PK_WAIT_CODE,     /* GETDB(MedicineCode) 응답 대기 */
    STEP_PK_WAIT_MEDINFO,  /* GETDB(MEDCODE) 응답 대기 - 약이름+채널 조회 */
    STEP_PK_WAIT_DONE,     /* STM32 배출 완료 대기 */
    STEP_PK_WAIT_LOG,      /* DISPENSE_LOG 응답 대기 */
    STEP_PK_WAIT_CUM,      /* CumMedicine 응답 대기 */
    STEP_PK_WAIT_CLEAR,    /* MedicineCode=0 응답 대기 */
    STEP_PK_WAIT_STOCK     /* Stock 감소 응답 대기 */
} Step;

/* 진행용 임시 변수 */
char pending_user[32]        = "";
char pending_symptom[32]     = "";
char pending_med[50]         = "";
char pending_requester[20]   = "";
char pending_allergy_tag[50] = "";
int  pending_code            = 0;
int  pending_count           = 0;
int  pending_age             = 0;
int  pending_min_age         = 0;
int  pending_base_count      = 0;
int  pending_teen_count      = 0;
int  pending_age_limit       = 0;
int  pending_slot            = 0;   // SlotNum
int  pending_servo           = 0;   // ServoChannel
Step g_step                  = STEP_NONE;

/* ══════════════════════════════════════════════════════
 * decide_count()
 * DB에서 받은 규칙 기반으로 처방 개수 결정
 * 반환: 처방 개수 (0이면 처방 불가)
 * ══════════════════════════════════════════════════════ */
static int decide_count(int age, int cum_medicine,
                        int base_count, int teen_count,
                        int age_limit,  int min_age)
{
    int result = 0;

    /* 1단계: 나이 기반 개수 결정 (DB 값 사용) */
    if (age_limit > 0 && age >= age_limit)
        result = base_count;    // 성인 개수
    else if (age >= min_age)
        result = teen_count;    // 청소년 개수
    else
        return 0;               // 나이 미달

    /* 2단계: OVERDOSE 방지 누적 기반 조정 */
    if (cum_medicine >= 20) {
        printf(" -> [OVERDOSE] CumMedicine=%d >= 20 → 처방 불가\n",
               cum_medicine);
        return 0;
    }
    else if (cum_medicine >= 10) {
        printf(" -> [OVERDOSE WARNING] CumMedicine=%d >= 10 → 최대 1알\n",
               cum_medicine);
        result = 1;
    }
    else {
        printf(" -> [Normal] CumMedicine=%d → %d알 처방\n",
               cum_medicine, result);
    }

    return result;
}

/* ══════════════════════════════════════════════════════
 * reset_pending()
 * ══════════════════════════════════════════════════════ */
static void reset_pending(void)
{
    pending_user[0]        = '\0';
    pending_symptom[0]     = '\0';
    pending_med[0]         = '\0';
    pending_requester[0]   = '\0';
    pending_allergy_tag[0] = '\0';
    pending_code        = 0;
    pending_count       = 0;
    pending_age         = 0;
    pending_min_age     = 0;
    pending_base_count  = 0;
    pending_teen_count  = 0;
    pending_age_limit   = 0;
    pending_slot        = 0;
    pending_servo       = 0;
    g_step              = STEP_NONE;
}

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

    close(sock);
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
    int* sock = (int*)arg;
    int  i, str_len;
    char* pToken;
    char* pArray[ARR_CNT];
    char  name_msg[NAME_SIZE + BUF_SIZE + 1];
    char  send_buf[BUF_SIZE];

    while (1) {
        memset(name_msg, 0, sizeof(name_msg));
        memset(pArray,   0, sizeof(pArray));

        str_len = read(*sock, name_msg, NAME_SIZE + BUF_SIZE);
        if (str_len <= 0) { *sock = -1; return NULL; }

        name_msg[str_len] = '\0';
        name_msg[strcspn(name_msg, "\r")] = '\0';
        name_msg[strcspn(name_msg, "\n")] = '\0';
        printf("[RECV] %s\n", name_msg);

        pToken = strtok(name_msg, "[:@]");
        i = 0;
        while (pToken != NULL) {
            pArray[i] = pToken;
            if (++i >= ARR_CNT) break;
            pToken = strtok(NULL, "[:@]");
        }
        if (i < 2) continue;

        /* ═══════════════════════════════════════════════
         * [흐름1-A] STM -> DOC : 예약 픽업 요청
         *   수신: [DOC]PICKUP@UserID
         * ═══════════════════════════════════════════════ */
        if (!strcmp(pArray[1], "PICKUP"))
        {
            strcpy(pending_requester, pArray[0]);
            if (i < 3) {
                snprintf(send_buf, sizeof(send_buf),
                         "[%s]PICKUP@FAIL_EMPTY@0\n", pending_requester);
                write(*sock, send_buf, strlen(send_buf));
                continue;
            }
            strcpy(pending_user, pArray[2]);
            printf("\n>>> [Pickup] From:%s | UserID:%s\n",
                   pending_requester, pending_user);

            g_step = STEP_PK_WAIT_CODE;
            snprintf(send_buf, sizeof(send_buf),
                     "[SQL]GETDB@USERID@%s@MedicineCode\n", pending_user);
            write(*sock, send_buf, strlen(send_buf));
        }

        /* ═══════════════════════════════════════════════
         * [흐름1-B] SQL -> DOC : GETDB(MedicineCode) 응답 (픽업)
         *   수신: [SQL]GETDB@UserID@MedicineCode@Code
         * ═══════════════════════════════════════════════ */
        else if (!strcmp(pArray[0], "SQL") && !strcmp(pArray[1], "GETDB")
                 && g_step == STEP_PK_WAIT_CODE)
        {
            if (i >= 4 && (!strcmp(pArray[3], "NOT_FOUND") ||
                           !strcmp(pArray[3], "FAIL"))) {
                printf(" -> [Pickup] 미등록 유저\n");
                snprintf(send_buf, sizeof(send_buf),
                         "[%s]PICKUP@FAIL_EMPTY@0\n", pending_requester);
                write(*sock, send_buf, strlen(send_buf));
                reset_pending();
                continue;
            }

            int code = (i >= 5) ? atoi(pArray[4]) : 0;
            if (code == 0) {
                printf(" -> [Pickup] 처방 없음\n");
                snprintf(send_buf, sizeof(send_buf),
                         "[%s]PICKUP@FAIL_EMPTY@0\n", pending_requester);
                write(*sock, send_buf, strlen(send_buf));
                reset_pending();
                continue;
            }

            pending_code  = code;
            pending_count = code % 100;    // 뒷자리 = 개수
            int med_type  = code / 100;    // 앞자리 = 약종류

            printf(" -> [Pickup] MedicineCode:%d MedType:%d 개수:%d → DB 조회\n",
                   code, med_type, pending_count);

            /* DB에서 약이름 + ServoChannel 조회 */
            g_step = STEP_PK_WAIT_MEDINFO;
            snprintf(send_buf, sizeof(send_buf),
                     "[SQL]GETDB@MEDCODE@%d\n", med_type);
            write(*sock, send_buf, strlen(send_buf));
        }

        /* ═══════════════════════════════════════════════
         * [흐름1-C] SQL -> DOC : GETDB(MEDCODE) 응답
         *   수신: [SQL]GETDB@MEDCODE@MedName@SlotNum@ServoChannel@Stock
         * ═══════════════════════════════════════════════ */
        else if (!strcmp(pArray[0], "SQL") && !strcmp(pArray[1], "GETDB")
                 && g_step == STEP_PK_WAIT_MEDINFO)
        {
            if (i < 4 || !strcmp(pArray[3], "NOT_FOUND")) {
                printf(" -> [Pickup] 약 정보 없음\n");
                snprintf(send_buf, sizeof(send_buf),
                         "[%s]PICKUP@FAIL_EMPTY@0\n", pending_requester);
                write(*sock, send_buf, strlen(send_buf));
                reset_pending();
                continue;
            }

            /* pArray[2]="MEDCODE" [3]=MedName [4]=SlotNum
             * pArray[5]=ServoChannel [6]=Stock */
            strcpy(pending_med, pArray[3]);
            pending_slot  = atoi(pArray[4]);
            pending_servo = atoi(pArray[5]);
            int stock     = atoi(pArray[6]);

            printf(" -> [Pickup] %s SlotNum:%d ServoChannel:%d Stock:%d 요청:%d\n",
                   pending_med, pending_slot, pending_servo,
                   stock, pending_count);

            /* 재고 확인 */
            if (stock < pending_count) {
                printf(" -> [Pickup] 재고 부족!\n");
                snprintf(send_buf, sizeof(send_buf),
                         "[%s]PICKUP@FAIL_STOCK@0\n", pending_requester);
                write(*sock, send_buf, strlen(send_buf));
                reset_pending();
                continue;
            }

            /* DB 변경 없이 STM32에 배출 승인 전송
             * ServoChannel 포함 → STM32가 채널 직접 사용 */
            g_step = STEP_PK_WAIT_DONE;
            snprintf(send_buf, sizeof(send_buf),
                     "[STM]PICKUP@%s@%d@%d\n",
                     pending_med, pending_count, pending_servo);
            write(*sock, send_buf, strlen(send_buf));
            snprintf(send_buf, sizeof(send_buf),"[ARD]PICKUP@%s@%d\n",   
            pending_med, pending_count);
            write(*sock, send_buf, strlen(send_buf));
        }

        /* ═══════════════════════════════════════════════
         * [흐름1-D] STM -> DOC : 배출 완료
         *   수신: [DOC]PICKUP_DONE@UserID@Med@Count
         *   → DB 업데이트 시작
         * ═══════════════════════════════════════════════ */
        else if (!strcmp(pArray[0], "STM") &&
                 !strcmp(pArray[1], "PICKUP_DONE") &&
                 g_step == STEP_PK_WAIT_DONE)
        {
            printf(" -> [Pickup] STM32 배출 완료 → DB 업데이트\n");
            g_step = STEP_PK_WAIT_LOG;
            snprintf(send_buf, sizeof(send_buf),
                     "[SQL]DISPENSE_LOG@%s@%d\n", pending_user, pending_code);
            write(*sock, send_buf, strlen(send_buf));
        }

        /* ═══════════════════════════════════════════════
         * [흐름1-E] STM -> DOC : 배출 실패
         *   수신: [DOC]PICKUP_FAIL@UserID
         *   → DB 유지, 재시도 가능
         * ═══════════════════════════════════════════════ */
        else if (!strcmp(pArray[0], "STM") &&
                 !strcmp(pArray[1], "PICKUP_FAIL") &&
                 g_step == STEP_PK_WAIT_DONE)
        {
            printf(" -> [Pickup] STM32 배출 실패 → DB 유지\n");
            reset_pending();
        }

        /* ═══════════════════════════════════════════════
         * [흐름2-1] AND/STM -> DOC : 증상 접수
         *   수신: [DOC]SYMPTOM@UserID@SymptomName
         *   → MedicineRule DB 조회 (하드코딩 없음)
         * ═══════════════════════════════════════════════ */
        else if (!strcmp(pArray[1], "SYMPTOM"))
        {
            strcpy(pending_requester, pArray[0]);
            if (i < 4) {
                snprintf(send_buf, sizeof(send_buf),
                         "[%s]PRESCRIPTION@FAIL_SYMPTOM@0\n", pending_requester);
                write(*sock, send_buf, strlen(send_buf));
                continue;
            }
            strcpy(pending_user,    pArray[2]);
            strcpy(pending_symptom, pArray[3]);

            printf("\n>>> [Consultation] From:%s | UserID:%s | Symptom:%s\n",
                   pending_requester, pending_user, pending_symptom);

            g_step = STEP_WAIT_MED_INFO;
            snprintf(send_buf, sizeof(send_buf),
                     "[SQL]GETDB@SYMPTOM@%s\n", pending_symptom);
            write(*sock, send_buf, strlen(send_buf));
        }

        /* ═══════════════════════════════════════════════
         * [흐름2-2] SQL -> DOC : MedicineRule 응답
         *   수신: [SQL]GETDB@증상@MedName@MedCode@MinAge
         *              @BaseCount@TeenCount@AgeLimit@AllergyTag
         * ═══════════════════════════════════════════════ */
        else if (!strcmp(pArray[0], "SQL") && !strcmp(pArray[1], "GETDB")
                 && g_step == STEP_WAIT_MED_INFO)
        {
            if (i < 4 || !strcmp(pArray[3], "NOT_FOUND")) {
                printf(" -> [Rejected] 증상 매칭 실패: %s\n", pending_symptom);
                snprintf(send_buf, sizeof(send_buf),
                         "[%s]PRESCRIPTION@FAIL_SYMPTOM@0\n", pending_requester);
                write(*sock, send_buf, strlen(send_buf));
                reset_pending();
                continue;
            }

            /* pArray[2]=증상 [3]=MedName [4]=MedCode [5]=MinAge
             * pArray[6]=BaseCount [7]=TeenCount [8]=AgeLimit [9]=AllergyTag */
            strcpy(pending_med,         pArray[3]);
            pending_code       = atoi(pArray[4]);
            pending_min_age    = atoi(pArray[5]);
            pending_base_count = atoi(pArray[6]);
            pending_teen_count = atoi(pArray[7]);
            pending_age_limit  = atoi(pArray[8]);
            strcpy(pending_allergy_tag, pArray[9] ? pArray[9] : "NONE");

            printf(" -> [MedInfo] %s Code:%d MinAge:%d Base:%d Teen:%d Limit:%d Allergy:%s\n",
                   pending_med, pending_code, pending_min_age,
                   pending_base_count, pending_teen_count,
                   pending_age_limit, pending_allergy_tag);

            /* 유저 알러지 조회 */
            g_step = STEP_WAIT_ALLERGY;
            snprintf(send_buf, sizeof(send_buf),
                     "[SQL]GETDB@USERID@%s@Allergy\n", pending_user);
            write(*sock, send_buf, strlen(send_buf));
        }

        /* ═══════════════════════════════════════════════
         * [흐름2-3] SQL -> DOC : 유저 알러지 응답
         *   수신: [SQL]GETDB@UserID@Allergy@NSAID
         * ═══════════════════════════════════════════════ */
        else if (!strcmp(pArray[0], "SQL") && !strcmp(pArray[1], "GETDB")
                 && g_step == STEP_WAIT_ALLERGY)
        {
            char *user_allergy = (i >= 5) ? pArray[4] : "NONE";

            printf(" -> [Allergy] 유저:%s / 약:%s\n",
                   user_allergy, pending_allergy_tag);

            if (strcmp(pending_allergy_tag, "NONE") != 0 &&
                strcmp(user_allergy, "NONE")          != 0 &&
                !strcmp(user_allergy, pending_allergy_tag))
            {
                printf(" -> [Rejected] 알러지 위험! %s\n", pending_allergy_tag);
                snprintf(send_buf, sizeof(send_buf),
                         "[%s]PRESCRIPTION@FAIL_ALLERGY@0\n", pending_requester);
                write(*sock, send_buf, strlen(send_buf));
                reset_pending();
                continue;
            }

            printf(" -> [OK] 알러지 이상 없음\n");
            g_step = STEP_WAIT_LIMIT;
            snprintf(send_buf, sizeof(send_buf),
                     "[SQL]CHECK_LIMIT@%s\n", pending_user);
            write(*sock, send_buf, strlen(send_buf));
        }

        /* ═══════════════════════════════════════════════
         * [흐름2-4] SQL -> DOC : CHECK_LIMIT_RES
         *   수신: [SQL]CHECK_LIMIT_RES@UserID@24h@Total@Age
         *   차단 순서:
         *     ① 24h >= 2       → FAIL_24H_LIMIT
         *     ② Total >= 30    → FAIL_TOTAL_LIMIT
         *     ③ decide_count() → 개수 결정 (DB 값 사용)
         * ═══════════════════════════════════════════════ */
        else if (!strcmp(pArray[0], "SQL") &&
                 !strcmp(pArray[1], "CHECK_LIMIT_RES") &&
                 g_step == STEP_WAIT_LIMIT)
        {
            if (i < 6 || !strcmp(pArray[3], "ERROR")) {
                printf(" -> [Error] 한도 조회 실패\n");
                snprintf(send_buf, sizeof(send_buf),
                         "[%s]PRESCRIPTION@FAIL_DB@0\n", pending_requester);
                write(*sock, send_buf, strlen(send_buf));
                reset_pending();
                continue;
            }

            int count_24h   = atoi(pArray[3]);
            int total_count = atoi(pArray[4]);
            int user_age    = atoi(pArray[5]);
            pending_age     = user_age;

            printf(" -> [Eligibility] 24H:%d / Cum:%d / Age:%d\n",
                   count_24h, total_count, user_age);

            /* ① 24h 한도 체크 */
            if (count_24h >= 2) {
                printf(" -> [Rejected] 24h 한도 초과\n");
                snprintf(send_buf, sizeof(send_buf),
                         "[%s]PRESCRIPTION@FAIL_24H_LIMIT@0\n", pending_requester);
                write(*sock, send_buf, strlen(send_buf));
                reset_pending();
                continue;
            }

            /* ② 누적 30알 이상 완전 차단 */
            if (total_count >= 30) {
                printf(" -> [Rejected] 누적 한도 초과 (%d >= 30)\n", total_count);
                snprintf(send_buf, sizeof(send_buf),
                         "[%s]PRESCRIPTION@FAIL_TOTAL_LIMIT@0\n", pending_requester);
                write(*sock, send_buf, strlen(send_buf));
                reset_pending();
                continue;
            }

            /* ③ 나이 + OVERDOSE 기반 개수 결정 (DB 값 사용) */
            int decided = decide_count(user_age, total_count,
                                       pending_base_count, pending_teen_count,
                                       pending_age_limit,  pending_min_age);
            if (decided == 0) {
                const char *fail_reason =
                    (total_count >= 20) ? "FAIL_OVERDOSE" : "FAIL_AGE_LIMIT";
                printf(" -> [Rejected] %s (Age:%d, Cum:%d)\n",
                       fail_reason, user_age, total_count);
                snprintf(send_buf, sizeof(send_buf),
                         "[%s]PRESCRIPTION@%s@0\n",
                         pending_requester, fail_reason);
                write(*sock, send_buf, strlen(send_buf));
                reset_pending();
                continue;
            }

            pending_count = decided;

            /* MedicineCode = 약종류*100 + 개수 */
            pending_code = (pending_code * 100) + pending_count;
            printf(" -> [OK] %s %d알 → MedicineCode=%d\n",
                   pending_med, pending_count, pending_code);

            /* 기존 예약 확인 */
            g_step = STEP_WAIT_RESV_CHECK;
            snprintf(send_buf, sizeof(send_buf),
                     "[SQL]GETDB@USERID@%s@MedicineCode\n", pending_user);
            write(*sock, send_buf, strlen(send_buf));
        }

        /* ═══════════════════════════════════════════════
         * [흐름2-5] SQL -> DOC : GETDB(MedicineCode) 기존 예약 확인
         * ═══════════════════════════════════════════════ */
        else if (!strcmp(pArray[0], "SQL") && !strcmp(pArray[1], "GETDB")
                 && g_step == STEP_WAIT_RESV_CHECK)
        {
            if (i >= 4 && (!strcmp(pArray[3], "NOT_FOUND") ||
                           !strcmp(pArray[3], "FAIL"))) {
                printf(" -> [Rejected] 미등록 유저: %s\n", pending_user);
                snprintf(send_buf, sizeof(send_buf),
                         "[%s]PRESCRIPTION@FAIL_DB@0\n", pending_requester);
                write(*sock, send_buf, strlen(send_buf));
                reset_pending();
                continue;
            }

            int cur_code = (i >= 5) ? atoi(pArray[4]) : 0;
            if (cur_code != 0) {
                printf(" -> [Rejected] 이미 예약 있음 (code=%d)\n", cur_code);
                snprintf(send_buf, sizeof(send_buf),
                         "[%s]PRESCRIPTION@FAIL_ALREADY_RESERVED@0\n",
                         pending_requester);
                write(*sock, send_buf, strlen(send_buf));
                reset_pending();
            } else {
                printf(" -> [OK] MedicineCode=%d 세팅\n", pending_code);
                g_step = STEP_WAIT_CODE;
                snprintf(send_buf, sizeof(send_buf),
                         "[SQL]UPDATEDB@%s@MedicineCode@%d\n",
                         pending_user, pending_code);
                write(*sock, send_buf, strlen(send_buf));
            }
        }

        /* ═══════════════════════════════════════════════
         * [공통] SQL -> DOC : UPDATEDB 응답
         * ═══════════════════════════════════════════════ */
        else if (!strcmp(pArray[0], "SQL") && !strcmp(pArray[1], "UPDATEDB"))
        {
            int ok_flag = (i >= 4 && !strcmp(pArray[3], "OK"));

            /* (픽업) CumMedicine 누적 → MedicineCode=0 초기화 */
            if (g_step == STEP_PK_WAIT_CUM) {
                printf(" -> [Pickup] CumMedicine %s → MedicineCode 초기화\n",
                       ok_flag ? "OK" : "WARN");
                g_step = STEP_PK_WAIT_CLEAR;
                snprintf(send_buf, sizeof(send_buf),
                         "[SQL]UPDATEDB@%s@MedicineCode@0\n", pending_user);
                write(*sock, send_buf, strlen(send_buf));
            }
            /* (픽업) MedicineCode=0 완료 → Stock 감소 */
            else if (g_step == STEP_PK_WAIT_CLEAR) {
                printf(" -> [Pickup] MedicineCode 초기화 %s → Stock 감소\n",
                       ok_flag ? "OK" : "WARN");
                g_step = STEP_PK_WAIT_STOCK;
                snprintf(send_buf, sizeof(send_buf),
                         "[SQL]UPDATEDB@%d@Stock@%d\n",
                         pending_slot, pending_count);
                write(*sock, send_buf, strlen(send_buf));
            }
            /* (픽업) Stock 감소 완료 → 픽업 완전 종료 */
            else if (g_step == STEP_PK_WAIT_STOCK) {
                printf(" -> [Pickup] 완료! %s x%d 처리 완료 (Stock %s)\n",
                       pending_med, pending_count,
                       ok_flag ? "감소 OK" : "감소 실패(재고 부족?)");
                snprintf(send_buf, sizeof(send_buf), "[ARD]PICKUP_DONE@%s@%s@%d\n",
                pending_user,pending_med,pending_count);
                write(*sock, send_buf, strlen(send_buf));
                usleep(100000);
                reset_pending();
            }
            /* (예약) MedicineCode 갱신 완료 → 처방 응답 */
            else if (g_step == STEP_WAIT_CODE) {
                if (ok_flag) {
                    printf(" -> [Reserved] %s x%d → %s 처방 완료\n",
                           pending_med, pending_count, pending_requester);
                    snprintf(send_buf, sizeof(send_buf),
                             "[%s]PRESCRIPTION@%s@%d\n",
                             pending_requester, pending_med, pending_count);
                } else {
                    printf(" -> [Error] MedicineCode 업데이트 실패\n");
                    snprintf(send_buf, sizeof(send_buf),
                             "[%s]PRESCRIPTION@FAIL_DB@0\n", pending_requester);
                }
                write(*sock, send_buf, strlen(send_buf));
                reset_pending();
            }
        }

        /* ═══════════════════════════════════════════════
         * SQL -> DOC : DISPENSE_LOG 응답
         *   → CumMedicine 누적 단계로
         * ═══════════════════════════════════════════════ */
        else if (!strcmp(pArray[0], "SQL") && !strcmp(pArray[1], "DISPENSE_LOG")
                 && g_step == STEP_PK_WAIT_LOG)
        {
            if (i >= 4 && !strcmp(pArray[3], "OK")) {
                printf(" -> [Pickup] 기록 완료 → CumMedicine+%d\n", pending_count);
                g_step = STEP_PK_WAIT_CUM;
                snprintf(send_buf, sizeof(send_buf),
                         "[SQL]UPDATEDB@%s@CumMedicine@%d\n",
                         pending_user, pending_count);
                write(*sock, send_buf, strlen(send_buf));
            } else {
                printf(" -> [Pickup] 기록 실패 → 중단\n");
                snprintf(send_buf, sizeof(send_buf),
                         "[%s]PICKUP@FAIL_DB@0\n", pending_requester);
                write(*sock, send_buf, strlen(send_buf));
                reset_pending();
            }
        }

        else {
            printf("[Ignored] %s\n", name_msg);
        }
    }
    return NULL;
}

void error_handling(char* msg)
{
    fputs(msg, stderr);
    fputc('\n', stderr);
    exit(1);
}