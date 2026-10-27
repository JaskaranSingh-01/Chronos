import { describe, expect, it } from 'vitest';
import { matches } from './symbols';

const day = ['A', 'AAPL', 'AMSC', 'AMSF', 'BRK.B', 'MS', 'MS-A', 'MSA', 'MSFT', 'MSGN', 'SMSI'];

describe('matches', () => {
  it('puts the symbol typed first, then those it starts, then those it is inside, ignoring case and spaces', () => {
    expect(matches(day, ' ms ')).toEqual({
      shown: ['MS', 'MSA', 'MSFT', 'MSGN', 'MS-A', 'AMSC', 'AMSF', 'SMSI'],
      total: 8,
    });
    expect(matches(day, 'brk.').shown).toEqual(['BRK.B']);
    expect(matches(day, 'ZZZ')).toEqual({ shown: [], total: 0 });
  });

  it('shows at most the limit, counts every match, and lists every symbol when nothing is typed', () => {
    expect(matches(day, 'a', 2)).toEqual({ shown: ['A', 'AAPL'], total: 6 });
    expect(matches(day, '', 3)).toEqual({ shown: ['A', 'AAPL', 'AMSC'], total: 11 });
  });
});
