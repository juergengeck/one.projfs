#!/usr/bin/env node

/**
 * This scripts allows connection to an edda.dev.refinio.one instance.
 * 
 * For this run this script, then with a running edda instance in a browser navigate to connections.
 * At the same time navifaget to the filesystem mount point -> invites/iom-invite and open the txt file.
 * Copy the link from there.
 * Alternativly the invite link to copy should be shown in the output of this script.
 * (Note that the IoP invite works, but is likely not what you want, as the filer instance has no functionility to accept chats or similar.)
 */

import fs from 'fs';
import path from 'path';
import os from 'os';
import { spawn } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Configuration - All test files in a single parent directory
const TEST_DIR = path.join(os.tmpdir(), 'one-filer-edda');
const MOUNT_POINT = path.join(TEST_DIR, 'mount');
const INVITES_PATH = path.join(MOUNT_POINT, 'invites');
const IOM_INVITE_FILE = path.join(INVITES_PATH, 'iom_invite.txt');

// Path to refinio.api (relative to one.projfs/test/integration/)
const REFINIO_API_DIR = path.resolve(__dirname, '../../refinio.api');
const SERVER_STORAGE_DIR = path.join(TEST_DIR, 'server-instance');
const COMM_SERVER = "wss://comm10.dev.refinio.one";

const SERVER_PORT = 3434

let serverProcess;

/**
 * Cleanup test environment
 *
 * IMPORTANT: This function aggressively cleans up ALL test state to prevent
 * caching issues. It must be called:
 * 1. At the START of tests (to clear stale state from previous runs)
 * 2. At the END of tests (to clean up after current run)
 * 3. On SIGINT/SIGTERM (to clean up on interruption)
 */
async function cleanupTestEnvironment() {
    console.log('🧹 Cleaning up test environment...');

    // Kill server process if running
    if (serverProcess) {
        try {
            serverProcess.kill('SIGINT');
            // Wait a bit for graceful shutdown
            await new Promise(resolve => setTimeout(resolve, 2000));
            if (!serverProcess.killed) {
                serverProcess.kill('SIGKILL');
            }
        } catch (err) {
            console.log('   Failed to kill server process:', err.message);
        }
        serverProcess = null;
    }

    // Wait for processes to fully exit and release file locks
    await new Promise(resolve => setTimeout(resolve, 1000));

    // Remove entire test directory with retry logic
    // This is CRITICAL to prevent instance cache issues that cause connection failures
    let retries = 3;
    while (retries > 0) {
        try {
            if (fs.existsSync(TEST_DIR)) {
                fs.rmSync(TEST_DIR, { recursive: true, force: true, maxRetries: 3, retryDelay: 500 });
                console.log(`   Removed ${TEST_DIR}`);
            }
            break;
        } catch (err) {
            retries--;
            if (retries === 0) {
                console.log(`   ⚠️  Failed to remove ${TEST_DIR} after retries:`, err.message);
                console.log(`   ⚠️  This may cause test failures due to cached instance state!`);
            } else {
                console.log(`   Retrying removal of ${TEST_DIR}... (${3 - retries}/3)`);
                await new Promise(resolve => setTimeout(resolve, 1000));
            }
        }
    }

    console.log('✅ Cleanup complete\n');
}

/**
 * Start refinio.api server with ProjFS mount
 */
async function startRefinioApiServer() {
    console.log('🚀 Starting refinio.api server with ProjFS...\n');

    // Verify refinio.api exists
    if (!fs.existsSync(REFINIO_API_DIR)) {
        throw new Error(`refinio.api not found at ${REFINIO_API_DIR}`);
    }

    const distIndexPath = path.join(REFINIO_API_DIR, 'dist', 'index.js');
    if (!fs.existsSync(distIndexPath)) {
        throw new Error(`refinio.api not built - missing ${distIndexPath}\n` +
                       `   Run: cd ${REFINIO_API_DIR} && npm run build`);
    }

    // Create mount point directory
    if (!fs.existsSync(MOUNT_POINT)) {
        fs.mkdirSync(MOUNT_POINT, { recursive: true });
        console.log(`   Created mount point: ${MOUNT_POINT}`);
    }

    console.log(`   Mount point: ${MOUNT_POINT}`);
    console.log(`   CommServer: ${COMM_SERVER}\n`);

    // Spawn server process with configuration via environment variables
    return new Promise((resolve, reject) => {
        serverProcess = spawn('node', [distIndexPath], {
            cwd: REFINIO_API_DIR,
            env: {
                ...process.env,
                // Server config
                REFINIO_API_HOST: '127.0.0.1',
                REFINIO_API_PORT: SERVER_PORT.toString(),
                // Instance config
                REFINIO_INSTANCE_NAME: 'edda-one-filer2',
                REFINIO_INSTANCE_DIRECTORY: SERVER_STORAGE_DIR,
                REFINIO_INSTANCE_EMAIL: 'edda-one-filer2@one.filer.test',
                REFINIO_INSTANCE_SECRET: 'server-secret-projfs-integration-12345678',
                REFINIO_COMM_SERVER_URL: `${COMM_SERVER}`,
                REFINIO_ENCRYPT_STORAGE: 'false',
                REFINIO_WIPE_STORAGE: 'true',
                // Filer config
                REFINIO_FILER_MOUNT_POINT: MOUNT_POINT,
                REFINIO_FILER_INVITE_URL_PREFIX: 'https://edda.dev.refinio.one/invites/inviteDevice/?invited=true/',
                REFINIO_FILER_DEBUG: 'true',
                // Other
                NODE_ENV: 'test'
            },
            stdio: ['ignore', 'pipe', 'pipe']
        });

        let serverOutput = '';
        let startupTimeout = null;

        // Collect output for debugging
        serverProcess.stdout.on('data', (data) => {
            const output = data.toString();
            serverOutput += output;
            process.stdout.write(output);  // Echo to console

            // Check for HTTP server ready
            // ProjFS mount may be synchronous or async, we'll poll the filesystem after HTTP is ready
            if (output.includes('HTTP REST API listening')) {
                clearTimeout(startupTimeout);
                console.log('\n✅ Server HTTP API ready, checking if ProjFS mount succeeded...\n');
                // Give ProjFS a moment to initialize, then we'll poll the filesystem
                setTimeout(() => resolve(), 2000);
            }
        });

        serverProcess.stderr.on('data', (data) => {
            const output = data.toString();
            serverOutput += output;
            process.stderr.write(output);  // Echo to console
        });

        serverProcess.on('error', (error) => {
            clearTimeout(startupTimeout);
            reject(new Error(`Failed to start server: ${error.message}`));
        });

        serverProcess.on('exit', (code) => {
            if (code !== 0 && code !== null) {
                clearTimeout(startupTimeout);
                reject(new Error(`Server exited with code ${code}\n${serverOutput}`));
            }
        });

        // Timeout after 60 seconds
        startupTimeout = setTimeout(() => {
            reject(new Error('Server startup timeout after 60 seconds\n' + serverOutput));
        }, 60000);
    });
}

/**
 * Main test function
 */
async function runConnectionTest() {
    console.log('🔗 ONE.projfs Connection Integration Test\n');
    console.log('=' .repeat(70));
    console.log(`Platform: Windows (ProjFS)`);
    console.log(`Test Directory: ${TEST_DIR}`);
    console.log(`Mount Point: ${MOUNT_POINT}`);
    console.log(`Invites Path: ${INVITES_PATH}\n`);

    // Setup: Clean up any existing test environment, start CommServer, then server
    try {
        await cleanupTestEnvironment();
        console.log('\n2️⃣ Starting SERVER instance with ProjFS...');
        await startRefinioApiServer();
    } catch (setupError) {
        console.error('\n❌ Setup Failed:', setupError.message);
        console.error('\n🔧 Troubleshooting:');
        console.error('   1. Ensure refinio.api is built: cd refinio.api && npm run build');
        console.error('   2. Check that ProjFS is available on Windows 10 1809+');
        console.error('   3. Verify you have permissions to mount ProjFS filesystems');
        throw setupError;
    }

    let testResults = {
        mountPointExists: false,
        invitesDirectoryExists: false,
        iopInviteExists: false,
        iomInviteExists: false,
        iopInviteReadable: false,
        iomInviteReadable: false,
        iopInviteValid: false,
        iomInviteValid: false,
        iopInviteSize: 0,
        iomInviteSize: 0
    };

    try {
        // Test 1: Check mount point exists
        console.log('\n3️⃣ Checking ProjFS mount point...');
        if (!fs.existsSync(MOUNT_POINT)) {
            throw new Error(`Mount point does not exist: ${MOUNT_POINT}\n` +
                           `   Please ensure ONE Filer is running with ProjFS enabled.\n` +
                           `   Set ONE_FILER_MOUNT environment variable if using different path.`);
        }
        testResults.mountPointExists = true;
        console.log(`✅ Mount point exists: ${MOUNT_POINT}`);

        // Read and validate IOM invite
        console.log('\n9️⃣ Reading and validating IOM invite...');
        let iomInviteContent;
        try {
            iomInviteContent = fs.readFileSync(IOM_INVITE_FILE, 'utf-8').trim();
            testResults.iomInviteReadable = true;
            testResults.iomInviteSize = iomInviteContent.length;
            console.log(`✅ IOM invite readable (${testResults.iomInviteSize} bytes)`);
        } catch (readError) {
            throw new Error(`Failed to read IOM invite: ${readError.message}`);
        }

        if (iomInviteContent.length === 0) {
            throw new Error('IOM invite file is empty!');
        }

        console.log(`IoM Invite URL: ${iomInviteContent}`)

        // Wait indefinitely until CTRL+C
        await new Promise(() => {});

    } catch (error) {
        console.error('\n❌ Filer Script Failed Failed:', error.message);
        process.exit(1);
    }
}

// Handle cleanup on signals
process.on('SIGINT', async () => {
    console.log('\n\n⚠️  Interrupted - cleaning up...');
    await cleanupTestEnvironment();
    process.exit(130);
});

process.on('SIGTERM', async () => {
    console.log('\n\n⚠️  Terminated - cleaning up...');
    await cleanupTestEnvironment();
    process.exit(143);
});

// Run the test
console.log('Starting edda filer instance\n');
runConnectionTest()
    .then(async () => {
        await cleanupTestEnvironment();
        process.exit(0);
    })
    .catch(async (error) => {
        if (error.stack) {
            console.error(error.stack);
        }
        await cleanupTestEnvironment();
        process.exit(1);
    });
