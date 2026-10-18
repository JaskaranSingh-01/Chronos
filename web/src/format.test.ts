import { describe, expect, it } from 'vitest';
import { clock, etOffset, money, px, seekTs, shares, signed, tone } from './format';

const open = 1_577_716_200; // 2019-12-30 09:30:00 ET, epoch seconds

describe('format', () => {
  it("finds New York's UTC offset, standard and daylight", () => {
    expect(etOffset(open)).toBe(-5 * 3_600);
    expect(etOffset(1_562_765_400)).toBe(-4 * 3_600); // 2019-07-10 09:30 EDT
  });

  it('shows an exchange time as New York wall-clock time to the millisecond', () => {
    expect(clock(open * 1e9 + 2_050_000)).toBe('09:30:00.002');
  });

  it('shows prices in cents unless they have finer ticks, and money in dollars', () => {
    expect([px(100_100, 4), px(100_150, 4), px(2_893_699, 4)]).toEqual(['10.01', '10.0150', '289.3699']);
    expect([money(-10_010_000, 4), signed(-10_000, 4), signed(124_000, 4), signed(0, 4)]).toEqual([
      '-$1,001.00',
      '-$1.00',
      '+$12.40',
      '$0.00',
    ]);
  });

  it('shows prices at any scale, whole or in cents, and negative ones', () => {
    expect([px(5, 0), px(1_234, 0)]).toEqual(['5', '1234']);
    expect([px(1, 2), px(1_001, 2), px(100_000, 2)]).toEqual(['0.01', '10.01', '1000.00']);
    expect([px(10_010, 3), px(10_015, 3), px(10_000, 3)]).toEqual(['10.01', '10.015', '10.00']);
    expect([px(0, 4), px(-100_100, 4), px(-100_150, 4), px(-1, 4)]).toEqual(['0.00', '-10.01', '-10.0150', '-0.0001']);
  });

  it('shows a loss below a cent as zero rather than minus zero, and a rebate with its plus', () => {
    expect([money(-1, 4), money(-49, 4), money(-51, 4), money(0, 4)]).toEqual(['$0.00', '$0.00', '-$0.01', '$0.00']);
    expect([signed(2_000, 4), signed(-2_000, 4), signed(-1, 4), signed(1, 4)]).toEqual([
      '+$0.20',
      '-$0.20',
      '$0.00',
      '$0.00',
    ]);
    expect(money(5_000_000, 4)).toBe('$500.00');
  });

  it('turns a New York wall-clock seek into exact epoch nanoseconds on the session day', () => {
    expect(seekTs(open * 1e9 + 123, '10:31:04')).toBe((open + 3_664) * 1e9);
    expect(seekTs(open * 1e9, '09:00')).toBe((open - 1_800) * 1e9);
    expect(Number.isSafeInteger(seekTs(open * 1e9, '16:00') / 512)).toBe(true); // a multiple of 512: exact in JSON
  });

  it('seeks in daylight time, and on the session day when the clock is already past midnight UTC', () => {
    const exact = (ns: number) => BigInt(ns);
    const summer = 1_562_765_400; // 2019-07-10 09:30:00 EDT
    expect(exact(seekTs(summer * 1e9, '09:30'))).toBe(BigInt(summer) * 1_000_000_000n);
    expect(exact(seekTs(summer * 1e9, '10:15:30'))).toBe(BigInt(summer + 2_730) * 1_000_000_000n);
    // 21:00 ET on 2019-12-30 is 02:00 UTC on the 31st: the session day is still the 30th
    const evening = Date.parse('2019-12-31T02:00:00Z') * 1e6;
    expect(exact(seekTs(evening, '09:30'))).toBe(BigInt(open) * 1_000_000_000n);
  });

  it('seeks to the start and the last second of the session day', () => {
    const midnight = BigInt(open - 34_200) * 1_000_000_000n; // 2019-12-30 00:00:00 ET
    expect(BigInt(seekTs(open * 1e9, '00:00'))).toBe(midnight);
    expect(BigInt(seekTs(open * 1e9, '23:59:59'))).toBe(midnight + 86_399n * 1_000_000_000n);
  });

  it('shows a share count signed, and colours a long buy and a short sell', () => {
    expect([shares(100), shares(-100), shares(0)]).toEqual(['+100', '-100', '0']);
    expect([tone(100), tone(-100), tone(0)]).toEqual(['buy', 'sell', '']);
  });
});
