module.exports = {
  preset: 'ts-jest',
  testEnvironment: 'node',
  roots: ['<rootDir>/tests'],
  testMatch: ['**/__tests__/**/*.+(ts|tsx|js)', '**/*.(test|spec).+(ts|tsx|js)'],
  globals: {
    'ts-jest': {
      tsconfig: {
        module: 'commonjs',
        target: 'es2018',
        lib: ['es2018'],
        moduleResolution: 'node',
        allowSyntheticDefaultImports: true,
        esModuleInterop: true,
        strict: false
      }
    }
  },
  collectCoverageFrom: [
    'IFSProjFSProvider.js',
    'index.js',
    '!**/node_modules/**',
    '!**/build/**',
  ],
  setupFilesAfterEnv: ['<rootDir>/tests/setup.ts'],
  testTimeout: 30000, // ProjFS operations can be slow
  verbose: true,
  detectOpenHandles: true,
  forceExit: true, // Needed for native module cleanup
};