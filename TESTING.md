# Testing Guide for one.projfs

This document describes how to test the one.projfs Windows ProjFS integration module.

## Prerequisites

### System Requirements

- **Windows 10 version 1809 or later** (October 2018 Update)
- **ProjFS feature enabled** (see [Enabling ProjFS](#enabling-projfs))
- **Visual Studio 2019+** with C++ build tools
- **Windows SDK** with ProjFS headers
- **Node.js 14+**

### Enabling ProjFS

Run PowerShell as Administrator:

```powershell
# Check if ProjFS is enabled
Get-WindowsOptionalFeature -Online -FeatureName Client-ProjFS

# Enable ProjFS if not already enabled
Enable-WindowsOptionalFeature -Online -FeatureName Client-ProjFS

# Restart your computer after enabling
```

### Building Dependencies

Before running tests, ensure all dependencies are built:

```bash
# Build one.core (required by one.models)
cd packages/one.core
npm run build

# Build one.models (provides PairingFileSystem, ConnectionsModel, etc.)
cd ../one.models
npm run build

# Build refinio.api (provides test server and client)
cd ../../refinio.api
npm run build

# Build one.projfs native module
cd ../one.projfs
npm install  # Automatically builds native module
```

## Running Tests

### Quick Test

```bash
cd one.projfs
npm test
```

### Clean Test (Recommended)

Always run tests with clean state to avoid caching issues:

```bash
npm run test:clean
```

This automatically:
1. Cleans all test directories
2. Removes cached instance state
3. Runs the integration test

### Manual Cleanup Before Testing

If tests are failing, manually clean state:

```bash
# Clean test state
npm run clean:test

# Run tests
npm test
```

## Integration Test Flow

The integration test (`test/integration/connection-test.js`) verifies:

1. ✅ **CommServer starts** - Local communication server on port 8000
2. ✅ **Server instance initializes** - refinio.api with ProjFS mount
3. ✅ **ProjFS mount succeeds** - Virtual filesystem at `C:\OneFiler-Test`
4. ✅ **Invite files accessible** - `/invites/iop_invite.txt` and `/invites/iom_invite.txt`
5. ✅ **Invite content valid** - Contains proper WebSocket URL, public key, and token
6. ✅ **Client instance starts** - Second refinio.api instance without mount
7. ✅ **Connection establishes** - Client connects to server using invite from ProjFS
8. ✅ **Bidirectional contacts** - Both instances create contacts for each other
9. ✅ **Cleanup completes** - Unmount, stop processes, clean directories

### Test Output

Successful test output looks like:

```
Starting one.projfs connection integration test...

🔗 ONE.projfs Connection Integration Test

1️⃣ Starting CommServer...
   ✅ CommServer started on localhost:8000

2️⃣ Starting SERVER instance with ProjFS...
   ✅ LeuteModel initialized
   ✅ ChannelManager initialized
   ✅ ConnectionsModel initialized - listening on CommServer
   ✅ Filesystem mounted at C:\OneFiler-Test

3️⃣ Checking ProjFS mount point...
   ✅ Mount point exists: C:\OneFiler-Test

4️⃣ Checking invites directory...
   ✅ Invites directory exists: C:\OneFiler-Test\invites

5️⃣ Checking IOP invite file...
   ✅ IOP invite file exists

6️⃣ Checking IOM invite file...
   ✅ IOM invite file exists

7️⃣ Reading and validating IOP invite...
   ✅ IOP invite readable (260 bytes)
   ✅ IOP invite is valid

8️⃣ Reading and validating IOM invite...
   ✅ IOM invite readable (260 bytes)
   ✅ IOM invite is valid

9️⃣ Verifying CommServer consistency...
   ✅ Both invites use same CommServer: ws://localhost:8000

🔟 Starting CLIENT refinio.api instance...
   ✅ Client HTTP API ready

1️⃣1️⃣ Establishing connection using invite from ProjFS mount...
   ✅ Invitation accepted successfully

1️⃣2️⃣ Verifying bidirectional contact creation...
   ✅ BIDIRECTIONAL CONTACT CREATION VERIFIED!
   ✅ Both instances can see each other as contacts

🎉 Final Results:
   ✅ ProjFS mount working correctly
   ✅ Invite files readable from real filesystem
   ✅ Connection established successfully
   ✅ Bidirectional contacts created
   ✅ Integration test PASSED!

✨ Connection integration test completed successfully!
```

## Common Test Failures and Solutions

### 1. Native Module Not Found

**Error**:
```
Error: The specified module could not be found.
build\Release\ifsprojfs.node
```

**Solution**:
```bash
# Enable ProjFS first (see Prerequisites)
# Then rebuild
npm rebuild
```

### 2. Connection Failures

**Error**:
```
No listening connection for the specified publicKey
```

**Root Cause**: Cached instance state from previous test runs.

**Solution**:
```bash
# ALWAYS clean before running tests
npm run clean:test
npm test
```

**Why this happens**:
- ONE instances cache cryptographic keys in `C:\Temp\refinio-api-server-instance`
- Stale keys prevent ConnectionsModel from properly registering with CommServer
- The server appears to listen, but uses old keys not matching new invitations

### 3. Invitation Token Mismatch

**Error**:
```
Token doGZA1-1q... but server has adMkep-QJ0x...
```

**Root Cause**: Bug in PairingFileSystem creating new invitation on every read.

**Solution**: Update to latest `@refinio/one.models` with the fix in `PairingFileSystem.getAndRefreshIopInviteIfNoneExists()`.

**What was fixed**:
```typescript
// BEFORE (broken):
async getAndRefreshIopInviteIfNoneExists() {
    const invite = await this.connectionsModel.pairing.createInvitation();
    this.iopInvite = invite;  // Always creates new!
    return invite;
}

// AFTER (fixed):
async getAndRefreshIopInviteIfNoneExists() {
    if (!this.iopInvite) {  // Check if exists
        const invite = await this.connectionsModel.pairing.createInvitation();
        this.iopInvite = invite;
    }
    return this.iopInvite;  // Reuse cached invitation
}
```

### 4. System Clock Issues

**Error**: Connection timeouts or "token expired"

**Solution**: Ensure system clock is accurate. CommServer uses timestamps for token validation.

### 5. Mount Point Busy

**Error**:
```
Mount point already exists: C:\OneFiler-Test
```

**Solution**:
```bash
# Remove lingering mount point
rmdir C:\OneFiler-Test

# Or use cleanup
npm run clean:test
```

### 6. Port Already in Use

**Error**:
```
EADDRINUSE: Port 50123 already in use
```

**Solution**:
```bash
# Kill lingering processes
taskkill /F /IM node.exe

# Or find specific process using port
netstat -ano | findstr :50123
taskkill /PID <PID> /F
```

## Test Configuration

### Environment Variables

You can customize test behavior:

```bash
# Change mount point
set ONE_FILER_MOUNT=D:\MyTestMount
npm test

# Enable debug logging
set NODE_DEBUG=one.projfs
npm test
```

### Test Timeouts

The integration test has built-in timeouts:

- **CommServer startup**: 10 seconds
- **Server initialization**: 30 seconds
- **Client initialization**: 30 seconds
- **Connection establishment**: 60 seconds
- **Total test timeout**: 120 seconds

### Test Directories

The test uses these directories (automatically cleaned):

| Directory | Purpose |
|-----------|---------|
| `C:\OneFiler-Test` | ProjFS mount point |
| `C:\Temp\refinio-api-server-instance` | Server ONE instance storage |
| `C:\Temp\refinio-api-client-instance` | Client ONE instance storage |

## Debugging Test Failures

### Enable Verbose Logging

Look for these log prefixes to understand what's happening:

```
[ProjFS]           - Native ProjFS operations
[Cache]            - Cache hits/misses and operations
[Native]           - C++ bridge operations
[PairingFileSystem]- Invitation creation and management
[ConnectionHandler]- Connection establishment
[ContactCreation]  - Contact creation process
[AccessRights]     - Access rights granting
```

### Check Instance State

If connection fails, check if instances initialized correctly:

```bash
# View server instance directory
dir C:\Temp\refinio-api-server-instance

# Should contain: <instance-hash>/
# Inside: heads/, maps/, blobs/, etc.

# If directory is corrupt or has stale state:
npm run clean:test
```

### Verify ProjFS Mount

```bash
# Check if mount point is accessible
dir C:\OneFiler-Test\invites

# Should show:
#   iom_invite.png
#   iom_invite.txt
#   iop_invite.png
#   iop_invite.txt
```

### Manual Test Steps

To manually verify each component:

1. **Build all packages** (see Building Dependencies)

2. **Start CommServer manually**:
   ```bash
   cd packages/one.models
   node lib/misc/ConnectionEstablishment/communicationServer/CommunicationServer.js
   ```

3. **Start refinio.api with ProjFS** in another terminal:
   ```bash
   cd refinio.api
   set REFINIO_INSTANCE_SECRET=test-secret-123
   set REFINIO_INSTANCE_EMAIL=test@example.com
   set REFINIO_COMM_SERVER_URL=ws://localhost:8000
   set REFINIO_FILER_MOUNT_POINT=C:\OneFiler-Test
   set REFINIO_WIPE_STORAGE=true
   node dist/index.js
   ```

4. **Check ProjFS mount**:
   ```bash
   dir C:\OneFiler-Test\invites
   type C:\OneFiler-Test\invites\iop_invite.txt
   ```

5. **Test connection** from third terminal:
   ```bash
   cd refinio.api
   # Copy invite URL from step 4
   curl -X POST http://localhost:49498/api/connections/invite ^
     -H "Content-Type: application/json" ^
     -d "{\"inviteUrl\":\"<paste-invite-url>\"}"
   ```

## CI/CD Integration

For automated testing in CI/CD pipelines:

```yaml
# Example GitHub Actions workflow
name: Test one.projfs

on: [push, pull_request]

jobs:
  test:
    runs-on: windows-latest

    steps:
      - uses: actions/checkout@v3

      - name: Setup Node.js
        uses: actions/setup-node@v3
        with:
          node-version: '18'

      - name: Enable ProjFS
        run: |
          Enable-WindowsOptionalFeature -Online -FeatureName Client-ProjFS -NoRestart
        shell: powershell

      - name: Install dependencies
        run: |
          cd packages/one.core && npm install && npm run build
          cd ../one.models && npm install && npm run build
          cd ../../refinio.api && npm install && npm run build
          cd ../one.projfs && npm install

      - name: Run tests
        run: |
          cd one.projfs
          npm run test:clean
```

## Test Maintenance

### When to Clean Cache

**Always clean** before running tests in these scenarios:

- After git pull/merge
- After rebuilding one.core or one.models
- After system restart
- After failed test runs
- When switching branches
- When debugging connection issues

### Best Practices

1. **Always use `npm run test:clean`** instead of `npm test`
2. **Never manually modify** test directories while tests are running
3. **Check ProjFS is enabled** before reporting test failures
4. **Ensure system clock is accurate** to avoid timestamp validation issues
5. **Rebuild native module** after pulling changes to C++ code
6. **Check for port conflicts** if using non-default ports

## Performance Testing

To test performance under load:

```bash
# Run multiple sequential tests
for /L %i in (1,1,10) do npm run test:clean

# Monitor cache performance
# Look for [Cache] HIT vs MISS ratios in output
```

Expected performance:
- Cache hit: < 1ms
- Cache miss + async fetch: 10-50ms
- Connection establishment: < 5 seconds
- Full test suite: < 60 seconds

## Reporting Issues

When reporting test failures, include:

1. **Test output** (full log)
2. **Windows version** (`winver`)
3. **ProjFS status** (`Get-WindowsOptionalFeature -Online -FeatureName Client-ProjFS`)
4. **Node.js version** (`node --version`)
5. **Clean test result** (after `npm run clean:test`)
6. **Steps to reproduce**

Example issue template:

```
### Environment
- Windows: 10.0.19045
- Node.js: v18.17.0
- ProjFS: Enabled
- one.projfs: 1.0.0

### Problem
Integration test fails with "No listening connection for the specified publicKey"

### Steps
1. npm run clean:test
2. npm test

### Output
[Paste full test output]

### Notes
- Happens consistently
- Tried after rebuild
- System clock is accurate
```
