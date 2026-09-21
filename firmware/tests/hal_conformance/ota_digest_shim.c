/**
 * @file ota_digest_shim.c
 * @brief 非 mock 平台下为 main.c 补齐 mock_ota_expected_digest 的链接期符号
 *
 * main.c 的 test_ota() 只在运行期 `hal()->platform_id == "mock"` 时才会调用
 * mock_ota_expected_digest（该函数定义于 platform/mock/mock_sys.c，供 mock
 * 平台自证 OTA 摘要校验逻辑）；对 gk7205v200 等真实平台，运行期走的是同一处
 * if/else 的 else 分支（digest 全零，注释已写明"测试框架后续接入 sha256
 * 实现"），永远不会调用到这个符号。
 *
 * 但 main.c 对它是无条件 extern 声明 + 直接调用，链接器无法感知"这个分支
 * 运行时不会走到"，仍会在链接期要求该符号存在——对任何不含 mock_sys.c 的
 * 平台库都会报 unresolved external symbol。这是 hal_conformance 这份共享测试
 * 文件本身的先天缺口（自初始提交起就存在，此前从未有非 mock 平台链接过它），
 * 不是本任务实现的问题，因此没有改 main.c 一个字节——只是通过 CMakeLists.txt
 * 按 IPC_PLATFORM 条件把这个占位实现编译进来，让链接得以完成。
 *
 * 函数体本身在 gk7205v200 上永远不会被执行到，内容是什么不影响任何断言。
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

void mock_ota_expected_digest(const void *data, size_t len, uint8_t out[32])
{
    (void)data; (void)len;
    memset(out, 0, 32);
}
