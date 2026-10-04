#pragma once

// 按键/上屏音效。**只有一种**反馈音：12ms 的合成"嗒"(频率下坠的正弦 + 起振噪声，
// 见 typing_click.cpp 的合成参数)，就是标准输入法按键反馈那个听感。不再有轴体采样
// 音色、也没有音色/亮度之类的档位，合成里连随机量都没有——每一声逐样本相同。
//
// play() 只把请求塞进队列就返回，合成与写 PWM 由一个后台任务做，UI 线程不会被
// 阻塞。忙(上一声未放完)则丢弃本次。仅 g_settings.typingClickEnabled() 为真时发声，
// 否则为 no-op。
void typingClickPlay(int count = 1);

// 试听：与 typingClickPlay 完全相同的一条路径，只是默认响三声。设置页里的
// "按键音效试听"用它。
void typingClickAudition(int count = 3);

// 立刻停掉放音、丢掉排队的声并打断正在播的那一声
// (关掉按键音效、开始录音前调用)。
void typingClickRelease();
