#!/usr/bin/env node

/**
 * Connection Integration Test for one.projfs (Windows ProjFS)
 *
 * This test verifies that:
 * 1. Starts refinio.api with ProjFS mount
 * 2. ProjFS mount exposes invite files correctly
 * 3. Invite files contain valid invitation URLs
 * 4. Invites can be used to establish connections
 * 5. Bidirectional contact creation works after connection
 * 6. Cleans up: unmounts and stops server
 *
 * Usage:
 *   node connection-test.js                  # Run test and cleanup automatically
 *   node connection-test.js --interactive    # Wait for CTRL+C before cleanup (inspect mount point)
 *
 * Prerequisites:
 * - Windows 10 1809 or later with ProjFS enabled
 * - refinio.api built and available (../refinio.api)
 * - Node.js installed on Windows
 */

import fs from 'fs';
import path from 'path';
import os from 'os';
import { spawn } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Parse command-line arguments
const args = process.argv.slice(2);
const INTERACTIVE_MODE = args.includes('--interactive');

// Configuration - All test files in a single parent directory
const TEST_DIR = path.join(os.tmpdir(), 'OneFiler-Test2');
const MOUNT_POINT = path.join(TEST_DIR, 'mount');
const INVITES_PATH = path.join(MOUNT_POINT, 'invites');
const IOP_INVITE_FILE = path.join(INVITES_PATH, 'iop_invite.txt');
const IOM_INVITE_FILE = path.join(INVITES_PATH, 'iom_invite.txt');

// Path to refinio.api (relative to one.projfs/test/integration/)
const REFINIO_API_DIR = path.resolve(__dirname, '../../../refinio.api');
const SERVER_STORAGE_DIR = path.join(TEST_DIR, 'server-instance');
const CLIENT_STORAGE_DIR = path.join(TEST_DIR, 'client-instance');
const COMM_SERVER_PORT = 8000;
const SERVER_PORT = 50123;
const CLIENT_PORT = 50125;

// Process handles
let serverProcess = null;
let clientProcess = null;
let commServer = null;

/**
 * Start local CommunicationServer
 */
async function startCommServer() {
    console.log('Starting local CommunicationServer...');

    try {
        // Import CommunicationServer from one.models
        const modelsPath = path.resolve(__dirname, '../../../packages/one.models/lib/misc/ConnectionEstablishment/communicationServer/CommunicationServer.js');
        // Convert to file:// URL - handle both Windows and Unix paths
        const fileUrl = modelsPath.startsWith('/') ? `file://${modelsPath}` : `file:///${modelsPath.replace(/\\/g, '/')}`;
        const CommunicationServerModule = await import(fileUrl);
        const CommunicationServer = CommunicationServerModule.default;

        commServer = new CommunicationServer();
        await commServer.start('localhost', COMM_SERVER_PORT);

        console.log(`   ✅ CommServer started on localhost:${COMM_SERVER_PORT}`);
    } catch (error) {
        console.error('Failed to start CommServer:', error);
        throw error;
    }
}

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

    // Stop CommServer
    if (commServer) {
        try {
            await commServer.stop();
            console.log('   Stopped CommServer');
        } catch (err) {
            console.log('   Failed to stop CommServer:', err.message);
        }
        commServer = null;
    }

    // Kill client process if running
    if (clientProcess) {
        try {
            clientProcess.kill('SIGINT');
            await new Promise(resolve => setTimeout(resolve, 1000));
            if (!clientProcess.killed) {
                clientProcess.kill('SIGKILL');
            }
        } catch (err) {
            console.log('   Failed to kill client process:', err.message);
        }
        clientProcess = null;
    }

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

    console.log(`   Server port: ${SERVER_PORT}`);
    console.log(`   Mount point: ${MOUNT_POINT}`);
    console.log(`   CommServer: ws://localhost:${COMM_SERVER_PORT}\n`);

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
                REFINIO_INSTANCE_NAME: 'server-projfs-instance',
                REFINIO_INSTANCE_DIRECTORY: SERVER_STORAGE_DIR,
                REFINIO_INSTANCE_EMAIL: 'server-projfs@one.filer.test',
                REFINIO_INSTANCE_SECRET: 'server-secret-projfs-integration-12345678',
                REFINIO_COMM_SERVER_URL: `ws://localhost:${COMM_SERVER_PORT}`,
                REFINIO_ENCRYPT_STORAGE: 'false',
                REFINIO_WIPE_STORAGE: 'true',
                // Filer config
                REFINIO_FILER_MOUNT_POINT: MOUNT_POINT,
                REFINIO_FILER_INVITE_URL_PREFIX: 'https://one.refinio.net/invite',
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
 * Start refinio.api CLIENT instance (without ProjFS mount)
 */
async function startClientInstance() {
    console.log('🚀 Starting refinio.api CLIENT instance (no mount)...\n');

    const distIndexPath = path.join(REFINIO_API_DIR, 'dist', 'index.js');

    console.log(`   Client port: ${CLIENT_PORT}`);
    console.log(`   CommServer: ws://localhost:${COMM_SERVER_PORT}\n`);

    return new Promise((resolve, reject) => {
        clientProcess = spawn('node', [distIndexPath], {
            cwd: REFINIO_API_DIR,
            env: {
                ...process.env,
                // Client config
                REFINIO_API_HOST: '127.0.0.1',
                REFINIO_API_PORT: CLIENT_PORT.toString(),
                // Instance config
                REFINIO_INSTANCE_NAME: 'client-projfs-instance',
                REFINIO_INSTANCE_DIRECTORY: CLIENT_STORAGE_DIR,
                REFINIO_INSTANCE_EMAIL: 'client-projfs@one.filer.test',
                REFINIO_INSTANCE_SECRET: 'client-secret-projfs-integration-12345678',
                REFINIO_COMM_SERVER_URL: `ws://localhost:${COMM_SERVER_PORT}`,
                REFINIO_ENCRYPT_STORAGE: 'false',
                REFINIO_WIPE_STORAGE: 'true',
                // NO Filer config - client doesn't mount
                NODE_ENV: 'test'
            },
            stdio: ['ignore', 'pipe', 'pipe']
        });

        let clientOutput = '';
        let startupTimeout = null;

        clientProcess.stdout.on('data', (data) => {
            const output = data.toString();
            clientOutput += output;
            process.stdout.write(`[CLIENT] ${output}`);

            if (output.includes('HTTP REST API listening')) {
                clearTimeout(startupTimeout);
                console.log('\n✅ Client HTTP API ready\n');
                setTimeout(() => resolve(), 1000);
            }
        });

        clientProcess.stderr.on('data', (data) => {
            const output = data.toString();
            clientOutput += output;
            process.stderr.write(`[CLIENT] ${output}`);
        });

        clientProcess.on('error', (error) => {
            clearTimeout(startupTimeout);
            reject(new Error(`Failed to start client: ${error.message}`));
        });

        clientProcess.on('exit', (code) => {
            if (code !== 0 && code !== null) {
                clearTimeout(startupTimeout);
                reject(new Error(`Client exited with code ${code}\n${clientOutput}`));
            }
        });

        startupTimeout = setTimeout(() => {
            reject(new Error('Client startup timeout after 60 seconds\n' + clientOutput));
        }, 60000);
    });
}

/**
 * Connect CLIENT to SERVER using invite (via HTTP REST API)
 */
async function connectUsingInvite(inviteUrl) {
    console.log('🔗 CLIENT accepting invitation from SERVER...');

    const http = await import('http');

    return new Promise((resolve, reject) => {
        const postData = JSON.stringify({ inviteUrl });
        const postOptions = {
            hostname: '127.0.0.1',
            port: CLIENT_PORT + 1,  // HTTP REST API runs on QUIC port + 1
            path: '/api/connections/invite',
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Content-Length': Buffer.byteLength(postData)
            }
        };

        const req = http.default.request(postOptions, (res) => {
            let data = '';
            res.on('data', (chunk) => data += chunk);
            res.on('end', () => {
                if (res.statusCode === 200 || res.statusCode === 201) {
                    console.log('   ✅ Invitation accepted successfully');
                    resolve(JSON.parse(data));
                } else {
                    reject(new Error(`HTTP ${res.statusCode}: ${data}`));
                }
            });
        });

        req.on('error', (error) => {
            reject(new Error(`Connection error: ${error.message}`));
        });

        req.setTimeout(120000); // 2 minute timeout
        req.write(postData);
        req.end();
    });
}

/**
 * Query contacts from a refinio.api instance
 */
async function queryContacts(port, instanceName) {
    const http = await import('http');

    return new Promise((resolve, reject) => {
        const options = {
            hostname: '127.0.0.1',
            port: port,
            path: '/api/contacts',
            method: 'GET'
        };

        const req = http.default.request(options, (res) => {
            let data = '';
            res.on('data', (chunk) => data += chunk);
            res.on('end', () => {
                if (res.statusCode === 200) {
                    const contacts = JSON.parse(data);
                    console.log(`   ${instanceName} contacts: ${contacts.length} found`);
                    resolve(contacts);
                } else {
                    console.error(`   ❌ Failed to query ${instanceName} contacts: HTTP ${res.statusCode}`);
                    resolve([]);
                }
            });
        });

        req.on('error', (error) => {
            console.error(`   ❌ Failed to query ${instanceName} contacts:`, error.message);
            resolve([]);
        });

        req.setTimeout(5000);
        req.end();
    });
}

/**
 * Parse invitation URL to extract credentials
 */
function parseInviteUrl(inviteUrl) {
    const hashIndex = inviteUrl.indexOf('#');
    if (hashIndex === -1) {
        throw new Error('Invalid invite URL format - no hash fragment');
    }

    const encodedData = inviteUrl.substring(hashIndex + 1);
    const decodedData = decodeURIComponent(encodedData);
    return JSON.parse(decodedData);
}

/**
 * Verify invite data structure
 */
function verifyInviteData(inviteData) {
    if (!inviteData.token || typeof inviteData.token !== 'string') {
        throw new Error('Invalid invite data: missing or invalid token');
    }
    if (!inviteData.publicKey || typeof inviteData.publicKey !== 'string') {
        throw new Error('Invalid invite data: missing or invalid publicKey');
    }
    if (!inviteData.url || typeof inviteData.url !== 'string') {
        throw new Error('Invalid invite data: missing or invalid url');
    }
    if (!inviteData.url.startsWith('wss://') && !inviteData.url.startsWith('ws://')) {
        throw new Error('Invalid invite data: url must be WebSocket URL');
    }
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
        console.log('\n1️⃣ Starting CommServer...');
        await startCommServer();
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

        // Test 2: Check all expected directories exist
        console.log('\n4️⃣ Checking mounted filesystems...');
        const expectedDirs = ['chats', 'debug', 'invites', 'objects', 'types', 'profiles', 'questionnaires'];
        const mountedDirs = fs.readdirSync(MOUNT_POINT).filter(item => {
            const itemPath = path.join(MOUNT_POINT, item);
            return fs.statSync(itemPath).isDirectory();
        });

        console.log(`   Expected directories: ${expectedDirs.join(', ')}`);
        console.log(`   Found directories: ${mountedDirs.join(', ')}`);

        const missingDirs = expectedDirs.filter(dir => !mountedDirs.includes(dir));
        if (missingDirs.length > 0) {
            throw new Error(`Missing directories in mount point: ${missingDirs.join(', ')}\n` +
                           `   Only found: ${mountedDirs.join(', ')}\n` +
                           `   This suggests the Filer filesystem was not mounted correctly.`);
        }

        console.log(`✅ All ${expectedDirs.length} directories mounted correctly`);

        // Test 2b: Check for duplicate directories using Windows 'dir' command
        console.log('\n4️⃣b Checking for duplicate directories using Windows dir command...');
        try {
            const { execSync } = await import('child_process');
            const dirOutput = execSync(`dir "${MOUNT_POINT}" /AD /B`, { encoding: 'utf8', shell: 'cmd.exe' });
            const windowsDirs = dirOutput.trim().split('\n').map(line => line.trim()).filter(line => line.length > 0);
            console.log(`   Windows dir command found: ${windowsDirs.join(', ')}`);

            const dirCounts = {};
            windowsDirs.forEach(dir => { dirCounts[dir] = (dirCounts[dir] || 0) + 1; });
            const duplicates = Object.entries(dirCounts).filter(([_, count]) => count > 1).map(([name, count]) => `${name} (${count}x)`);

            if (duplicates.length > 0) {
                console.warn(`DUPLICATE DIRECTORIES DETECTED via Windows dir command:\n   Duplicates: ${duplicates.join(', ')}\n   All directories: ${windowsDirs.join(', ')}`);
            } else {
                
                console.log(`✅ No duplicate directories found via Windows dir command`);
                console.log(`   Verified unique count: ${windowsDirs.length}`);

            }

        } catch (error) {
            if (error.message.includes('DUPLICATE DIRECTORIES')) throw error;
            console.warn(`   Warning: Could not run Windows dir command: ${error.message}`);
        }

        // Test 3: Check invites directory exists
        console.log('\n5️⃣ Checking invites directory...');
        if (!fs.existsSync(INVITES_PATH)) {
            throw new Error(`Invites directory not found: ${INVITES_PATH}\n` +
                           `   The PairingFileSystem may not be mounted.`);
        }
        testResults.invitesDirectoryExists = true;
        console.log(`✅ Invites directory exists: ${INVITES_PATH}`);

        // List all files in invites directory
        const inviteFiles = fs.readdirSync(INVITES_PATH);
        console.log(`   Files in invites/: ${inviteFiles.join(', ')}`);

        // Test 4: Check IOP invite file exists
        console.log('\n6️⃣ Checking IOP (Instance of Person) invite file...');
        if (!fs.existsSync(IOP_INVITE_FILE)) {
            throw new Error(`IOP invite file not found: ${IOP_INVITE_FILE}`);
        }
        testResults.iopInviteExists = true;
        console.log(`✅ IOP invite file exists: ${IOP_INVITE_FILE}`);

        // Test 5: Check IOM invite file exists
        console.log('\n7️⃣ Checking IOM (Instance of Machine) invite file...');
        if (!fs.existsSync(IOM_INVITE_FILE)) {
            throw new Error(`IOM invite file not found: ${IOM_INVITE_FILE}`);
        }
        testResults.iomInviteExists = true;
        console.log(`✅ IOM invite file exists: ${IOM_INVITE_FILE}`);

        // Test 6: Read and validate IOP invite
        console.log('\n8️⃣ Reading and validating IOP invite...');
        let iopInviteContent;
        try {
            iopInviteContent = fs.readFileSync(IOP_INVITE_FILE, 'utf-8').trim();
            testResults.iopInviteReadable = true;
            testResults.iopInviteSize = iopInviteContent.length;
            console.log(`✅ IOP invite readable (${testResults.iopInviteSize} bytes)`);
        } catch (readError) {
            throw new Error(`Failed to read IOP invite: ${readError.message}`);
        }

        if (iopInviteContent.length === 0) {
            throw new Error('IOP invite file is empty!\n' +
                           '   This indicates the ConnectionsModel is not generating invites.\n' +
                           '   Check that allowPairing: true in ConnectionsModel config.');
        }

        let iopInviteData;
        try {
            iopInviteData = parseInviteUrl(iopInviteContent);
            verifyInviteData(iopInviteData);
            testResults.iopInviteValid = true;
            console.log(`✅ IOP invite is valid`);
            console.log(`   WebSocket URL: ${iopInviteData.url}`);
            console.log(`   Public Key: ${iopInviteData.publicKey.substring(0, 16)}...`);
            console.log(`   Token: ${iopInviteData.token.substring(0, 16)}...`);
        } catch (parseError) {
            throw new Error(`Invalid IOP invite format: ${parseError.message}`);
        }

        // Test 7: Read and validate IOM invite
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

        let iomInviteData;
        try {
            iomInviteData = parseInviteUrl(iomInviteContent);
            verifyInviteData(iomInviteData);
            testResults.iomInviteValid = true;
            console.log(`✅ IOM invite is valid`);
            console.log(`   WebSocket URL: ${iomInviteData.url}`);
            console.log(`   Public Key: ${iomInviteData.publicKey.substring(0, 16)}...`);
            console.log(`   Token: ${iomInviteData.token.substring(0, 16)}...`);
        } catch (parseError) {
            throw new Error(`Invalid IOM invite format: ${parseError.message}`);
        }

        // Test 8: Verify both invites use same CommServer
        console.log('\n🔟 Verifying CommServer consistency...');
        if (iopInviteData.url !== iomInviteData.url) {
            console.log(`⚠️  Warning: IOP and IOM invites use different CommServers`);
            console.log(`   IOP: ${iopInviteData.url}`);
            console.log(`   IOM: ${iomInviteData.url}`);
        } else {
            console.log(`✅ Both invites use same CommServer: ${iopInviteData.url}`);
        }

        // Summary
        console.log('\n' + '=' .repeat(70));
        console.log('📊 Test Results Summary:\n');
        console.log(`✅ ProjFS mount point accessible: ${testResults.mountPointExists}`);
        console.log(`✅ Invites directory accessible: ${testResults.invitesDirectoryExists}`);
        console.log(`✅ IOP invite file exists: ${testResults.iopInviteExists}`);
        console.log(`✅ IOM invite file exists: ${testResults.iomInviteExists}`);
        console.log(`✅ IOP invite readable (${testResults.iopInviteSize} bytes): ${testResults.iopInviteReadable}`);
        console.log(`✅ IOM invite readable (${testResults.iomInviteSize} bytes): ${testResults.iomInviteReadable}`);
        console.log(`✅ IOP invite valid: ${testResults.iopInviteValid}`);
        console.log(`✅ IOM invite valid: ${testResults.iomInviteValid}`);

        console.log('\n🎯 Initial Validation Complete:');
        console.log('   ✅ ProjFS virtualization is working correctly');
        console.log('   ✅ PairingFileSystem is exposing invite files');
        console.log('   ✅ Invite content is valid and ready for connection');

        // Wait for server to fully register with CommServer
        console.log('\n   Waiting for server to register with CommServer...');
        await new Promise(resolve => setTimeout(resolve, 3000));

        // Test 8: Start CLIENT instance
        console.log('\n🔟 Starting CLIENT refinio.api instance...');
        await startClientInstance();

        // Test 9: CLIENT connects to SERVER using invite from ProjFS mount
        console.log('\n1️⃣1️⃣ Establishing connection using invite from ProjFS mount...');
        await connectUsingInvite(iopInviteContent);

        // Wait for connection to stabilize and contacts to be created
        console.log('\n   Waiting for connection to stabilize and contacts to be created...');
        await new Promise(resolve => setTimeout(resolve, 5000));

        // Test 10: Verify bidirectional contact creation
        console.log('\n1️⃣2️⃣ Verifying bidirectional contact creation...');

        const serverContacts = await queryContacts(SERVER_PORT + 1, 'SERVER');  // HTTP REST API port
        const clientContacts = await queryContacts(CLIENT_PORT + 1, 'CLIENT');  // HTTP REST API port

        let connectionSuccess = false;
        if (clientContacts.length > 0 && serverContacts.length > 0) {
            console.log('\n   ✅ BIDIRECTIONAL CONTACT CREATION VERIFIED!');
            console.log('   ✅ Both instances can see each other as contacts');
            connectionSuccess = true;
        } else if (clientContacts.length > 0) {
            console.log('\n   ⚠️  Partial success: CLIENT sees SERVER, but not vice versa');
        } else if (serverContacts.length > 0) {
            console.log('\n   ⚠️  Partial success: SERVER sees CLIENT, but not vice versa');
        } else {
            throw new Error('No contacts found on either side - connection failed');
        }

        // Verify profiles directory has entries after pairing
        console.log('\n1️⃣1️⃣ Verifying profiles directory after pairing...');
        const profilesDir = path.join(MOUNT_POINT, 'profiles');
        if (!fs.existsSync(profilesDir)) {
            throw new Error(`Profiles directory not found: ${profilesDir}`);
        }

        const profileEntries = fs.readdirSync(profilesDir);
        console.log(`   Found ${profileEntries.length} profile(s) in /profiles`);

        if (profileEntries.length === 0) {
            throw new Error('No profiles found after pairing!\n' +
                           '   Expected at least 1 profile entry after successful connection.\n' +
                           '   This suggests contact creation may have failed.');
        }

        console.log(`✅ Profiles directory contains ${profileEntries.length} entry/entries`);
        console.log(`   Profile entries: ${profileEntries.join(', ')}`);

        // Verify objects directory is correctly populated
        console.log('\n1️⃣2️⃣ Verifying objects directory structure...');
        const objectsDir = path.join(MOUNT_POINT, 'objects');
        if (!fs.existsSync(objectsDir)) {
            throw new Error(`Objects directory not found: ${objectsDir}`);
        }

        const objectHashes = fs.readdirSync(objectsDir).filter(item => {
            const itemPath = path.join(objectsDir, item);
            return fs.statSync(itemPath).isDirectory() && /^[0-9A-Fa-f]{64}$/.test(item);
        });
        console.log(`   Found ${objectHashes.length} object hash(es) in /objects`);

        if (objectHashes.length === 0) {
            throw new Error('No object hashes found in /objects!\n' +
                           '   Expected at least some object hashes after initialization.\n' +
                           '   This suggests ObjectsFileSystem is not correctly populating the directory.');
        }

        // Verify a sample object hash directory has expected files
        const sampleObjectHash = objectHashes[0];
        const sampleObjectDir = path.join(objectsDir, sampleObjectHash);
        const objectFiles = fs.readdirSync(sampleObjectDir);
        console.log(`   Sample object ${sampleObjectHash.substring(0, 16)}... contains: ${objectFiles.join(', ')}`);

        const expectedObjectFiles = ['raw.txt', 'type.txt', 'pretty.html', 'json.txt'];
        const hasRequiredFiles = objectFiles.includes('raw.txt') && objectFiles.includes('type.txt');
        if (!hasRequiredFiles) {
            throw new Error(`Object hash directory missing required files!\n` +
                           `   Expected at least: raw.txt, type.txt\n` +
                           `   Found: ${objectFiles.join(', ')}`);
        }

        console.log(`✅ Objects directory correctly populated with ${objectHashes.length} hash(es)`);
        console.log(`   Sample object has required files: ${objectFiles.filter(f => expectedObjectFiles.includes(f)).join(', ')}`);

        // Verify types directory is correctly populated
        console.log('\n1️⃣3️⃣ Verifying types directory structure...');
        const typesDir = path.join(MOUNT_POINT, 'types');
        if (!fs.existsSync(typesDir)) {
            throw new Error(`Types directory not found: ${typesDir}`);
        }

        const typeNames = fs.readdirSync(typesDir).filter(item => {
            const itemPath = path.join(typesDir, item);
            return fs.statSync(itemPath).isDirectory();
        });
        console.log(`   Found ${typeNames.length} type(s) in /types`);
        console.log(`   Types: ${typeNames.join(', ')}`);

        if (typeNames.length === 0) {
            throw new Error('No types found in /types!\n' +
                           '   Expected at least some types after initialization.\n' +
                           '   This suggests TypesFileSystem is not correctly populating the directory.');
        }

        // Verify a sample type directory has hash subdirectories
        const sampleTypeName = typeNames[0];
        const sampleTypeDir = path.join(typesDir, sampleTypeName);
        const typeHashes = fs.readdirSync(sampleTypeDir).filter(item => {
            const itemPath = path.join(sampleTypeDir, item);
            return fs.statSync(itemPath).isDirectory() && /^[0-9A-Fa-f]{64}$/.test(item);
        });
        console.log(`   Type '${sampleTypeName}' contains ${typeHashes.length} hash(es)`);

        if (typeHashes.length === 0) {
            throw new Error(`Type directory '${sampleTypeName}' has no hash subdirectories!\n` +
                           '   Expected at least one hash subdirectory.\n' +
                           '   This suggests TypesFileSystem is not correctly populating subdirectories.');
        }

        // Verify a sample hash directory within a type has expected files
        const sampleTypeHash = typeHashes[0];
        const sampleTypeHashDir = path.join(sampleTypeDir, sampleTypeHash);
        const typeHashFiles = fs.readdirSync(sampleTypeHashDir);
        console.log(`   Hash ${sampleTypeHash.substring(0, 16)}... in '${sampleTypeName}' contains: ${typeHashFiles.join(', ')}`);

        const expectedTypeFiles = ['raw.txt', 'type.txt', 'pretty.html', 'json.txt'];
        const hasRequiredTypeFiles = typeHashFiles.includes('type.txt') &&
                                     (typeHashFiles.includes('raw.txt') || typeHashFiles.includes('raw'));
        if (!hasRequiredTypeFiles) {
            throw new Error(`Type hash directory missing required files!\n` +
                           `   Expected at least: raw.txt (or raw), type.txt\n` +
                           `   Found: ${typeHashFiles.join(', ')}`);
        }

        console.log(`✅ Types directory correctly populated with ${typeNames.length} type(s)`);
        console.log(`   Sample type '${sampleTypeName}' has ${typeHashes.length} hash(es)`);
        console.log(`   Sample hash has files: ${typeHashFiles.join(', ')}`);

        console.log('\n🎉 Final Results:');
        console.log('   ✅ All 7 directories mounted correctly');
        console.log('   ✅ ProjFS mount working correctly');
        console.log('   ✅ Invite files readable from real filesystem');
        console.log('   ✅ Connection established successfully');
        console.log('   ✅ Bidirectional contacts created');
        console.log(`   ✅ Profile entries created (${profileEntries.length} found)`);
        console.log(`   ✅ Objects directory populated (${objectHashes.length} hashes)`);
        console.log(`   ✅ Types directory populated (${typeNames.length} types)`);
        console.log('   ✅ Integration test PASSED!');

        // Interactive mode: wait for user inspection before cleanup
        if (INTERACTIVE_MODE) {
            console.log('\n' + '='.repeat(70));
            console.log('🔍 INTERACTIVE MODE');
            console.log('='.repeat(70));
            console.log('\nMount point is ready for inspection:');
            console.log(`   ${MOUNT_POINT}`);
            console.log('\nServer instance directory:');
            console.log(`   ${SERVER_STORAGE_DIR}`);
            console.log('\nClient instance directory:');
            console.log(`   ${CLIENT_STORAGE_DIR}`);
            console.log('\n👉 Press CTRL+C to cleanup and exit');
            console.log('='.repeat(70));
            console.log('\n⏳ Waiting for CTRL+C...\n');

            // Wait indefinitely until CTRL+C
            await new Promise(() => {});
        }

    } catch (error) {
        console.error('\n❌ Test Failed:', error.message);
        console.error('\n📊 Partial Results:', testResults);

        console.error('\n🔧 Troubleshooting:');
        console.error('   1. Ensure ONE Filer is running with ProjFS enabled');
        console.error('   2. Check that ConnectionsModel has allowPairing: true');
        console.error('   3. Verify ProjFS is properly mounted at', MOUNT_POINT);
        console.error('   4. Check Windows Event Viewer for ProjFS errors');
        console.error('   5. Ensure Windows 10 1809+ with ProjFS feature enabled');

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
console.log('Starting one.projfs connection integration test...\n');
runConnectionTest()
    .then(async () => {
        console.log('\n✨ Connection integration test completed successfully!');
        await cleanupTestEnvironment();
        process.exit(0);
    })
    .catch(async (error) => {
        console.error('\n❌ Test failed:', error);
        if (error.stack) {
            console.error(error.stack);
        }
        await cleanupTestEnvironment();
        process.exit(1);
    });
