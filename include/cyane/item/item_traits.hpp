#pragma once

#include <cstdint>
#include <optional>

namespace cyane::item {

// 1.12.2 食物恢复的血量（半点为 1，满 20）。未列出的不是食物。
// 饥饿系统未建模前，进食直接回血。
[[nodiscard]] inline std::optional<int> food_heal(std::int16_t item_id) {
    switch (item_id) {
        case 260: return 4;    // 苹果
        case 297: return 5;    // 面包
        case 319: return 3;    // 生猪排
        case 320: return 8;    // 熟猪排
        case 363: return 3;    // 生牛肉
        case 364: return 8;    // 牛排
        case 365: return 2;    // 生鸡肉
        case 366: return 6;    // 熟鸡肉
        case 349: return 2;    // 生鱼
        case 350: return 5;    // 熟鱼
        case 391: return 3;    // 胡萝卜
        case 392: return 1;    // 马铃薯
        case 393: return 5;    // 烤马铃薯
        case 400: return 9;    // 南瓜派
        case 282: return 8;    // 蘑菇煲
        case 322: return 20;   // 金苹果（直接回满）
        case 357: return 2;    // 曲奇
        case 360: return 4;    // 西瓜片
        case 412: return 3;    // 生兔肉
        case 413: return 5;    // 熟兔肉
        case 423: return 2;    // 生羊肉
        case 424: return 6;    // 熟羊肉
        case 411: return 2;    // 生兔腿? 411=raw mutton 备用
        default: return std::nullopt;
    }
}

// 手持攻击伤害（点数）。1.12.2 剑/工具/徒手的基础值。
[[nodiscard]] inline float attack_damage(std::int16_t item_id) {
    switch (item_id) {
        case 268: return 4.0f;  // 木剑
        case 272: return 5.0f;  // 石剑
        case 267: return 6.0f;  // 铁剑
        case 276: return 7.0f;  // 钻石剑
        case 283: return 4.0f;  // 金剑
        case 271: return 3.0f;  // 木斧
        case 275: return 4.0f;  // 石斧
        case 258: return 5.0f;  // 铁斧
        case 279: return 6.0f;  // 钻斧
        case 286: return 3.0f;  // 金斧
        case 270: return 2.0f;  // 木镐
        case 274: return 3.0f;  // 石镐
        case 257: return 4.0f;  // 铁镐
        case 278: return 5.0f;  // 钻镐
        case 285: return 2.0f;  // 金镐
        case 269: return 1.5f;  // 木锹
        case 273: return 2.5f;  // 石锹
        case 256: return 3.5f;  // 铁锹
        case 277: return 4.5f;  // 钻锹
        case 284: return 1.5f;  // 金锹
        default: return 1.0f;   // 徒手与其他物品
    }
}

} // namespace cyane::item
