/*pc端运行进行逻辑测试*/
#include <stdio.h>
#include <string.h>
#include "frame_protocol.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) \
    do \
    { \
        if(cond) \
        { \
            g_pass++; \
            printf("[pass] %s\n", msg); \
        } \
        else \
        { \
            g_fail++; \
            printf("[fail] %s  (line %d)\n", msg, __LINE__); \
        } \
    } \
    while(0)

//向状态解析器喂字节
static frame_result_t feed_bytes(frame_parser_t *p, const uint8_t *data, int n, frame_t *out)
{
    frame_result_t last = FRAME_NONE;
    for(int i = 0; i < n; i++)
    {
        frame_result_t r = frame_parser_feed(p, data[i], out);
        //在一帧完整解析完后返回结果
        if(r != FRAME_NONE)
        {
            last = r;
        }
    }
    return last;
}
//温湿度单帧测试
static void test_single_frame(void)
{
    printf("[1]正常单帧(温湿度)\n");
    frame_parser_t p;
    frame_parser_init(&p);
    frame_t f;
    uint8_t th[4] = {0x01, 0x2A, 0x02, 0x58};
    uint8_t buf[16];

    //
    int n = frame_build(CMD_TEMP_HUM, th, 4, buf, sizeof(buf));
    CHECK(n == 9,"组帧长度 = 4+5 = 9");
    
    //用f来存放解析后的数据
    frame_result_t r = feed_bytes(&p, buf, n, &f);
    CHECK(r == FRAME_OK,"解析出FRAME_OK");
    CHECK(f.cmd == CMD_TEMP_HUM, "cmd正确");
    CHECK(f.len == 4, "len正确");
    CHECK(memcmp(f.payload, th, 4) == 0, "payload完全一致");
    CHECK(p.st_frames == 1 && p.st_err_sum == 0, "统计:1帧,0错误");
}

/*测试心跳帧*/
static void test_heartbeat(void)
{
    printf("[2]心跳帧 LEN=0\n");
    frame_parser_t p;
    frame_parser_init(&p);
    frame_t f;
    uint8_t buf[8];

    int n = frame_build(CMD_HEARTBEAT, NULL, 0, buf, sizeof(buf));
    CHECK(n == 5,"组帧长度= 0+5=5");

    frame_result_t r = feed_bytes(&p, buf, n, &f);
    CHECK(r == FRAME_OK,"len=0能正常跳过data进行校验");
    CHECK(f.cmd == CMD_HEARTBEAT && f.len == 0,"cmd/len正确");
}
/*测试最大负载*/
static void test_max_payload(void)
{
    printf("  [3] 最大负载 64 字节\n");
    frame_parser_t p; frame_parser_init(&p);
    frame_t f;
    uint8_t data[FRAME_MAX_PAYLOAD];
    uint8_t buf[FRAME_MAX_PAYLOAD + 8];

    for (int i = 0; i < FRAME_MAX_PAYLOAD; i++) data[i] = (uint8_t)i;

    int n = frame_build(CMD_OLED_TEXT, data, FRAME_MAX_PAYLOAD, buf, sizeof(buf));
    frame_result_t r = feed_bytes(&p, buf, n, &f);

    CHECK(r == FRAME_OK, "64 字节负载正常解析");
    CHECK(f.len == FRAME_MAX_PAYLOAD, "len = 64");
    CHECK(memcmp(f.payload, data, FRAME_MAX_PAYLOAD) == 0, "负载一致");
}

/*测试一次喂半包*/
static void test_split_frame(void)
{
    printf("  [4] 半包（分 3 次喂入）\n");
    frame_parser_t p; frame_parser_init(&p);
    frame_t f;
    uint8_t th[4] = {0x00, 0xC8, 0x02, 0x58};
    uint8_t buf[16];

    int n = frame_build(CMD_TEMP_HUM, th, 4, buf, sizeof(buf));

    CHECK(feed_bytes(&p, buf, 2, &f) == FRAME_NONE,       "只喂 2 字节：不出帧");
    CHECK(feed_bytes(&p, buf + 2, 3, &f) == FRAME_NONE,   "再喂 3 字节：仍不出帧");
    CHECK(feed_bytes(&p, buf + 5, n - 5, &f) == FRAME_OK, "补齐后出帧");
    CHECK(f.cmd == CMD_TEMP_HUM, "跨多次调用状态保持正确");
}

/*测试一次喂两帧*/
static void test_two_frames(void)
{
    printf("  [5] 粘包（连续两帧）\n");
    frame_parser_t p; frame_parser_init(&p);
    frame_t f;
    uint8_t b1[16], b2[16], stream[32];
    uint8_t t1[4] = {0x01, 0x2A, 0x02, 0x58};
    uint8_t t2[4] = {0x00, 0xFA, 0x02, 0x76};

    int n1 = frame_build(CMD_TEMP_HUM, t1, 4, b1, sizeof(b1));
    int n2 = frame_build(CMD_TEMP_HUM, t2, 4, b2, sizeof(b2));
    memcpy(stream, b1, n1);
    memcpy(stream + n1, b2, n2);

    int ok_count = 0;
    for (int i = 0; i < n1 + n2; i++)
    {
        if (frame_parser_feed(&p, stream[i], &f) == FRAME_OK) ok_count++;
    }
    CHECK(ok_count == 2, "一次喂两帧 → 解析出 2 帧");
    CHECK(p.st_frames == 2, "统计帧数 = 2");
}

/*篡改sum*/
static void test_bad_checksum(void)
{
    printf("  [6] 校验错误(SUM 被篡改）\n");
    frame_parser_t p; frame_parser_init(&p);
    frame_t f;
    uint8_t th[4] = {0x01, 0x2A, 0x02, 0x58};
    uint8_t buf[16];

    int n = frame_build(CMD_TEMP_HUM, th, 4, buf, sizeof(buf));
    buf[n - 1] ^= 0xFF;                     /* 破坏校验字节 */

    frame_result_t r = feed_bytes(&p, buf, n, &f);
    CHECK(r == FRAME_ERR_CHECKSUM, "返回 FRAME_ERR_CHECKSUM");
    CHECK(p.st_err_sum == 1 && p.st_frames == 0, "统计：0 帧、1 次校验错");
}

/*超长len*/
static void test_oversize_len(void)
{
    printf("  [7] 超长 LEN（非法长度保护）\n");
    frame_parser_t p; frame_parser_init(&p);
    frame_t f;

    /* 手工构造 LEN=200（远超 64 上限）的帧头 */
    uint8_t bad[5] = {FRAME_HEAD1, FRAME_HEAD2, 200, 0x01, 0x00};

    frame_result_t r = feed_bytes(&p, bad, 5, &f);
    CHECK(r == FRAME_ERR_LEN, "返回 FRAME_ERR_LEN");
    CHECK(p.st_err_len == 1, "统计长度错误 = 1");

    /* 关键：非法帧之后必须能立刻恢复，正常处理后续数据 */
    uint8_t th[4] = {0x01, 0x2A, 0x02, 0x58};
    uint8_t good[16];
    int n = frame_build(CMD_TEMP_HUM, th, 4, good, sizeof(good));
    CHECK(feed_bytes(&p, good, n, &f) == FRAME_OK, "错误帧后能恢复正常解析");
}

/*噪声前缀*/
static void test_noise_prefix(void)
{
    printf("  [8] 噪声前缀（线上干扰后重新同步）\n");
    frame_parser_t p; frame_parser_init(&p);
    frame_t f;
    uint8_t th[4] = {0x01, 0x2A, 0x02, 0x58};
    uint8_t good[16];

    int n = frame_build(CMD_TEMP_HUM, th, 4, good, sizeof(good));

    /* 先灌 20 个随机垃圾字节，再发正常帧 */
    uint8_t noise[20];
    for (int i = 0; i < 20; i++) noise[i] = (uint8_t)(0x10 + i * 7);

    feed_bytes(&p, noise, 20, &f);
    CHECK(feed_bytes(&p, good, n, &f) == FRAME_OK, "噪声后仍能锁定帧头");
    CHECK(f.cmd == CMD_TEMP_HUM, "解析正确");
}

/*负载里包含帧头*/
static void test_header_in_payload(void)
{
    printf("  [9] 负载内含 0xAA 0x55(不能误判为帧头)\n");
    frame_parser_t p; frame_parser_init(&p);
    frame_t f;
    uint8_t tricky[5] = {FRAME_HEAD1, FRAME_HEAD2, FRAME_HEAD1, FRAME_HEAD2, 0x42};
    uint8_t buf[16];

    int n = frame_build(CMD_OLED_TEXT, tricky, 5, buf, sizeof(buf));
    frame_result_t r = feed_bytes(&p, buf, n, &f);

    CHECK(r == FRAME_OK, "正常解析出一帧");
    CHECK(f.len == 5 && memcmp(f.payload, tricky, 5) == 0,
          "负载里的帧头字节被当作数据（靠 LEN 划界）");
}

/*发一半截断帧*/
static void test_truncated(void)
{
    printf("  [10] 截断帧（发出一半后线路静默）\n");
    frame_parser_t p; frame_parser_init(&p);
    frame_t f;
    uint8_t th[4] = {0x01, 0x2A, 0x02, 0x58};
    uint8_t buf[16];

    int n = frame_build(CMD_TEMP_HUM, th, 4, buf, sizeof(buf));
    frame_result_t r = feed_bytes(&p, buf, n - 2, &f);   /* 少喂 2 字节 */

    CHECK(r == FRAME_NONE, "不出帧（数据不完整）");
    CHECK(p.st_frames == 0, "不计入成功帧数");
    /* ★ 上层检测到帧间隔（STM32 的 IDLE 中断 / ESP32 的读超时）后
     *   必须调用 resync 丢弃半帧，否则紧随的完整帧会被错位吞掉。 */
    frame_parser_resync(&p);
    CHECK(p.st_dropped == 1, "半帧丢弃计数 = 1");
    CHECK(feed_bytes(&p, buf, n, &f) == FRAME_OK, "resync 后完整帧正常解析");
}

/* ---------- [11] AA AA 55 边界（原实现的真实缺陷） ---------- */
static void test_double_head(void)
{
    printf("  [11] AA AA 55 边界（连续帧头字节）\n");
    frame_parser_t p; frame_parser_init(&p);
    frame_t f;
    uint8_t th[4] = {0x01, 0x2A, 0x02, 0x58};
    uint8_t good[16];

    int n = frame_build(CMD_TEMP_HUM, th, 4, good, sizeof(good));

    /* 模拟：前一个帧头只到了一半就被打断，紧接着新帧头到来
     *   AA | AA 55 09 01 ... */
    frame_parser_feed(&p, FRAME_HEAD1, &f);                  /* 孤立的首字节 */
    frame_result_t r = feed_bytes(&p, good, n, &f);          /* 完整帧紧跟其后 */

    CHECK(r == FRAME_OK, "AA AA 55... 能正确识别（不漏帧）");
    CHECK(f.cmd == CMD_TEMP_HUM && f.len == 4, "解析正确");
}

/*多实例互不干扰 */
static void test_multi_parser(void)
{
    printf("  [12] 多实例独立（模拟双串口）\n");
    frame_parser_t p1, p2;
    frame_parser_init(&p1);
    frame_parser_init(&p2);
    frame_t f;
    uint8_t t1[4] = {0x01, 0x2A, 0x02, 0x58};
    uint8_t t2[4] = {0x00, 0xFA, 0x02, 0x76};
    uint8_t b1[16], b2[16];

    int n1 = frame_build(CMD_TEMP_HUM, t1, 4, b1, sizeof(b1));
    int n2 = frame_build(CMD_LED, t2, 4, b2, sizeof(b2));

    /* 交错喂给两个解析器（模拟两个串口交替来数据） */
    for (int i = 0; i < n1 || i < n2; i++)
    {
        if (i < n1) frame_parser_feed(&p1, b1[i], &f);
        if (i < n2) frame_parser_feed(&p2, b2[i], &f);
    }
    CHECK(p1.st_frames == 1 && p2.st_frames == 1, "两个实例各自解析出 1 帧");
    CHECK(p1.st_err_sum == 0 && p2.st_err_sum == 0, "互不干扰、无错误");
}

int main(void)
{
    printf("=== 帧协议单元测试 ===\n\n");

    test_single_frame();
    test_heartbeat();
    test_max_payload();
    test_split_frame();
    test_two_frames();
    test_bad_checksum();
    test_oversize_len();
    test_noise_prefix();
    test_header_in_payload();
    test_truncated();
    test_double_head();
    test_multi_parser();

    printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;      /* 返回非 0 让 CTest 判定失败 */
}