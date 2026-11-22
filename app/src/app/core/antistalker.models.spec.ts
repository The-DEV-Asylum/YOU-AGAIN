import {
  Candidate,
  candidateStateClass,
  displayState,
  filterCandidates,
  formatBytes,
  formatLastSeen,
  formatUptime,
  isElevated,
  activeCount,
  distanceMeters,
} from './antistalker.models';

function candidate(overrides: Partial<Candidate> = {}): Candidate {
  return {
    id: 'A1B2C3D4',
    type: 'Apple Find My',
    state: 'NEARBY',
    mac: 'AA:BB:CC:DD:EE:FF',
    addressType: 1,
    rssi: -64,
    peak: -52,
    sightings: 9,
    minutes: 4,
    locations: 1,
    owner: 'unknown',
    lastSeen: 2,
    allowed: false,
    ...overrides,
  };
}

describe('candidate presentation and filtering', () => {
  it('uses CO-PRESENT only for personal/general devices', () => {
    const following = candidate({ state: 'FOLLOWING' });
    expect(displayState(following, 'device')).toBe('CO-PRESENT');
    expect(displayState(following, 'tracker')).toBe('FOLLOWING');
  });

  it('recognizes elevated states and allow-list presentation', () => {
    expect(isElevated(candidate())).toBe(false);
    expect(isElevated(candidate({ state: 'PERSISTENT' }))).toBe(true);
    expect(candidateStateClass(candidate({ state: 'FOLLOWING' }))).toBe('following');
    expect(candidateStateClass(candidate({ state: 'FOLLOWING', allowed: true }))).toBe('allowed');
  });

  it('treats trusted devices as allowed for presentation', () => {
    expect(candidateStateClass(candidate({ trusted: true }))).toBe('allowed');
    const trusted = candidate({ trusted: true, trustedName: 'My phone' });
    expect(filterCandidates([trusted], 'device', 'my phone', 'all')).toHaveLength(1);
    expect(filterCandidates([trusted], 'device', 'trusted', 'all')).toHaveLength(1);
  });

  it('searches all visible fields including the device display state', () => {
    const list = [candidate({ type: 'Phone', state: 'FOLLOWING' })];
    expect(filterCandidates(list, 'device', 'co-present', 'all')).toHaveLength(1);
    expect(filterCandidates(list, 'device', 'aa:bb', 'all')).toHaveLength(1);
    expect(filterCandidates(list, 'device', 'AirTag', 'all')).toHaveLength(0);
  });

  it('filters attention without hiding a separate allowed filter', () => {
    const list = [
      candidate({ id: '1', state: 'PERSISTENT' }),
      candidate({ id: '2', state: 'FOLLOWING', allowed: true }),
      candidate({ id: '3' }),
    ];
    expect(filterCandidates(list, 'tracker', '', 'attention').map((item) => item.id)).toEqual(['1']);
    expect(filterCandidates(list, 'tracker', '', 'allowed').map((item) => item.id)).toEqual(['2']);
  });

  it('sorts active elevated observations before allowed observations', () => {
    const list = [
      candidate({ id: 'allowed', state: 'FOLLOWING', allowed: true }),
      candidate({ id: 'nearby' }),
      candidate({ id: 'following', state: 'FOLLOWING' }),
    ];
    expect(filterCandidates(list, 'tracker', '', 'all').map((item) => item.id)).toEqual([
      'following',
      'nearby',
      'allowed',
    ]);
  });
});

describe('time formatting', () => {
  it('formats recent observations and device uptime', () => {
    expect(formatLastSeen(1)).toBe('Now');
    expect(formatLastSeen(42)).toBe('42s ago');
    expect(formatLastSeen(180)).toBe('3m ago');
    expect(formatUptime(59)).toBe('0m 59s');
    expect(formatUptime(3720)).toBe('1h 2m');
    expect(formatUptime(90000)).toBe('1d 1h');
  });
});

describe('device discovery and health helpers', () => {
  it('counts only recently transmitting candidates as active', () => {
    const list = [
      candidate({ id: '1', lastSeen: 2 }),
      candidate({ id: '2', lastSeen: 119 }),
      candidate({ id: '3', lastSeen: 300 }),
      candidate({ id: '4', lastSeen: 3600 }),
    ];
    expect(activeCount(list)).toBe(2);
    expect(activeCount(list, 60)).toBe(1);
  });

  it('measures great-circle distance in meters', () => {
    // The classic value: ~111.2 km per degree of latitude.
    const latOnly = distanceMeters(52.0, 13.0, 53.0, 13.0);
    expect(latOnly).toBeGreaterThan(111_000);
    expect(latOnly).toBeLessThan(112_000);
    expect(distanceMeters(52.0, 13.0, 52.0, 13.0)).toBe(0);
  });

  it('formats byte counts for the system panel', () => {
    expect(formatBytes(0)).toBe('0');
    expect(formatBytes(2048)).toBe('2.0 KiB');
    expect(formatBytes(8 * 1024 * 1024)).toBe('8.0 MiB');
  });
});
