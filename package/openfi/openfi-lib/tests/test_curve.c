/* openfi_curve 通用模板测试 —— 顺便当"这模板怎么用"的示例 */
#define _GNU_SOURCE
#include "../src/openfi_curve.h"
#include "t.h"

#include <stdio.h>

int main(void)
{
	struct curve c;
	struct curve_hyst h;

	/* 用法:CURVE_LOAD(&表, 模式, min_y, max_y, 紧急x, 紧急y, {点}...)
	   点数由 sizeof 自动算,加点不用改数字 */
	printf("── 阶梯模式(风扇曲线那种)──\n");
	CURVE_LOAD(&c, CURVE_STEP, 0, 100, 85, 100,
		   {55,5},{58,36},{61,68},{65,100});
	T_EQ_INT(curve_valid(&c), 1);
	T_EQ_INT(curve_eval(&c, 20), 5);
	T_EQ_INT(curve_eval(&c, 55), 5);
	T_EQ_INT(curve_eval(&c, 56), 36);
	T_EQ_INT(curve_eval(&c, 58), 36);
	T_EQ_INT(curve_eval(&c, 59), 68);
	T_EQ_INT(curve_eval(&c, 61), 68);
	T_EQ_INT(curve_eval(&c, 62), 100);
	T_EQ_INT(curve_eval(&c, 84), 100);
	T_EQ_INT(curve_eval(&c, 85), 100);     /* 紧急值 */
	T_EQ_INT(curve_eval(&c, 200), 100);

	printf("── 线性模式(同一模板换个模式)──\n");
	CURVE_LOAD(&c, CURVE_LINEAR, 0, 100, 0, 0, {0,0},{100,100});
	T_EQ_INT(curve_eval(&c, 0), 0);
	T_EQ_INT(curve_eval(&c, 25), 25);
	T_EQ_INT(curve_eval(&c, 50), 50);
	T_EQ_INT(curve_eval(&c, 100), 100);
	CURVE_LOAD(&c, CURVE_LINEAR, 0, 100, 0, 0, {0,0},{50,10},{100,100});
	T_EQ_INT(curve_eval(&c, 25), 5);       /* 0→50 从 0 涨到 10 */
	T_EQ_INT(curve_eval(&c, 75), 55);      /* 50→100 从 10 涨到 100 */

	printf("── 另一个真实用例:RSRP(dBm) → 信号格数 ──\n");
	CURVE_LOAD(&c, CURVE_STEP, 0, 5, 0, 0,
		   {-140,0},{-120,1},{-110,2},{-100,3},{-90,4},{-80,5});
	T_EQ_INT(curve_eval(&c, -141), 0);
	T_EQ_INT(curve_eval(&c, -125), 1);
	T_EQ_INT(curve_eval(&c, -115), 2);
	T_EQ_INT(curve_eval(&c, -105), 3);
	T_EQ_INT(curve_eval(&c, -95), 4);
	T_EQ_INT(curve_eval(&c, -80), 5);
	T_EQ_INT(curve_eval(&c, -50), 5);

	printf("── 夹取 ──\n");
	CURVE_LOAD(&c, CURVE_STEP, 0, 100, 0, 0, {0,0},{10,200});
	T_EQ_INT(curve_eval(&c, 10), 100);     /* 表里的 200 被 max_y 夹住 */

	printf("── 校验:各种非法表 ──\n");
	CURVE_LOAD(&c, CURVE_STEP, 0, 100, 0, 0, {0,0},{10,10});
	T_EQ_INT(curve_valid(&c), 1);
	CURVE_LOAD(&c, CURVE_STEP, 0, 100, 0, 0, {10,0},{0,10});
	T_EQ_INT(curve_valid(&c), 0);          /* x 没递增 */
	CURVE_LOAD(&c, CURVE_STEP, 0, 100, 0, 0, {0,10},{10,5});
	T_EQ_INT(curve_valid(&c), 0);          /* y 下降 */
	CURVE_LOAD(&c, CURVE_STEP, 50, 100, 0, 0, {0,0},{10,100});
	T_EQ_INT(curve_valid(&c), 0);          /* 表里有低于 min_y 的点 */
	CURVE_LOAD(&c, CURVE_STEP, 0, 100, 0, 0, {0,0});
	T_EQ_INT(curve_valid(&c), 0);          /* 只有 1 个点 */
	CURVE_LOAD(&c, CURVE_STEP, 0, 100, 5, 100, {0,0},{10,10});
	T_EQ_INT(curve_valid(&c), 0);          /* 紧急值落在最后一个点之前 */
	T_EQ_INT(curve_valid(NULL), 0);
	T_EQ_INT(curve_load(NULL, NULL, 0, CURVE_STEP, 0, 100, 0, 0), -1);
	T_EQ_INT(curve_eval(NULL, 5), 0);

	printf("── 迟滞 ──\n");
	curve_hyst_init(&h, 10, 5000);
	T_EQ_INT(curve_hyst_should_apply(&h, 50, 1000), 1);   /* 首次 */
	curve_hyst_applied(&h, 50, 1000);
	T_EQ_INT(h.last_y, 50);
	T_EQ_INT(curve_hyst_should_apply(&h, 51, 1100), 1);   /* 上冲立即 */
	T_EQ_INT(curve_hyst_should_apply(&h, 45, 999999), 0); /* 回落 5 < 10 */
	T_EQ_INT(curve_hyst_should_apply(&h, 40, 999999), 1); /* 回落 10 */
	curve_hyst_init(&h, 1, 5000);
	curve_hyst_applied(&h, 50, 1000);
	T_EQ_INT(curve_hyst_should_apply(&h, 20, 3000), 0);   /* 间隔不够 */
	T_EQ_INT(curve_hyst_should_apply(&h, 20, 6000), 1);
	curve_hyst_applied(&h, 50, 4000);                     /* 同值不刷新时间 */
	T_EQ_INT(h.last_ms, 1000);
	T_EQ_INT(curve_hyst_should_apply(NULL, 1, 1), 1);
	curve_hyst_applied(NULL, 1, 1);

	T_SUMMARY();
}
