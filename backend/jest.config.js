module.exports = {
  preset: 'ts-jest',
  testEnvironment: 'node',
  rootDir: '.',
  testMatch: ['<rootDir>/test/**/*.spec.ts'],
  // Integration tests start real containers' clients and a Nest app; the
  // default 5s is not enough on a cold CI runner.
  testTimeout: 30000,
};
