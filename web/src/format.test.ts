import { describe, expect, it } from 'vitest';
import { etOffset } from './format';

const open = 1_577_716_200; // 2019-12-30 09:30:00 ET, epoch seconds

describe('format', () => {
  it("finds New York's UTC offset, standard and daylight", () => {
    expect(etOffset(open)).toBe(-5 * 3_600);
    expect(etOffset(1_562_765_400)).toBe(-4 * 3_600); // 2019-07-10 09:30 EDT
  });
});
