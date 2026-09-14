/* stb_image 解码实现实例化（决-1：FM225 UVC MJPG 预览软解，2026-09-14）。
 * 实现只放本翻译单元，且本文件属 safe_thirdparty（-w），不污染全仓 0 告警目标。
 * 只开 JPEG 解码路径（预览源固定 MJPEG），禁用 stdio（只从内存解码）；
 * 禁用 HDR/float 路径（STBI_NO_HDR/STBI_NO_LINEAR）：该路径引用 libm 的 pow，
 * 会让所有链接 safe_thirdparty 的测试（如 test_event_bus）被迫带上 -lm。 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

