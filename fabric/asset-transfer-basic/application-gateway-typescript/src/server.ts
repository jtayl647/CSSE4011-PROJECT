/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * HTTP REST server for irrigation system blockchain interface.
 *
 * POST /reading        - submit a sensor reading to the blockchain
 * GET  /readings       - get all readings from the blockchain
 * GET  /readings/:node - get all readings for a specific node
 */

import * as grpc from '@grpc/grpc-js';
import { connect, hash, Identity, Signer, signers } from '@hyperledger/fabric-gateway';
import * as crypto from 'crypto';
import express, { Request, Response } from 'express';
import cors from 'cors';
import { promises as fs } from 'fs';
import * as path from 'path';
import { TextDecoder } from 'util';

// =============================================================================
// Fabric connection config
// =============================================================================
const channelName   = envOrDefault('CHANNEL_NAME',    'mychannel');
const chaincodeName = envOrDefault('CHAINCODE_NAME',  'irrigation');
const mspId         = envOrDefault('MSP_ID',          'Org1MSP');
const PORT          = parseInt(envOrDefault('PORT',   '3000'));

const cryptoPath       = envOrDefault('CRYPTO_PATH', path.resolve(__dirname, '..', '..', '..', 'test-network', 'organizations', 'peerOrganizations', 'org1.example.com'));
const keyDirectoryPath = envOrDefault('KEY_DIRECTORY_PATH', path.resolve(cryptoPath, 'users', 'User1@org1.example.com', 'msp', 'keystore'));
const certDirectoryPath = envOrDefault('CERT_DIRECTORY_PATH', path.resolve(cryptoPath, 'users', 'User1@org1.example.com', 'msp', 'signcerts'));
const tlsCertPath      = envOrDefault('TLS_CERT_PATH', path.resolve(cryptoPath, 'peers', 'peer0.org1.example.com', 'tls', 'ca.crt'));
const peerEndpoint     = envOrDefault('PEER_ENDPOINT',    'localhost:7051');
const peerHostAlias    = envOrDefault('PEER_HOST_ALIAS',  'peer0.org1.example.com');

const utf8Decoder = new TextDecoder();

// =============================================================================
// Bootstrap — connect to Fabric then start HTTP server
// =============================================================================
async function main(): Promise<void> {
    const client  = await newGrpcConnection();
    const gateway = connect({
        client,
        identity: await newIdentity(),
        signer:   await newSigner(),
        hash:     hash.sha256,
        evaluateOptions:     () => ({ deadline: Date.now() + 5000 }),
        endorseOptions:      () => ({ deadline: Date.now() + 15000 }),
        submitOptions:       () => ({ deadline: Date.now() + 5000 }),
        commitStatusOptions: () => ({ deadline: Date.now() + 60000 }),
    });

    const network  = gateway.getNetwork(channelName);
    const contract = network.getContract(chaincodeName);

    const app = express();
    app.use(cors());
    app.use(express.json());

    // -------------------------------------------------------------------------
    // POST /reading
    // Body: { nodeId, timestamp, moisture, humidity, temperature, pressure }
    // Called by the base node WiFi board when it receives data from the mobile.
    // -------------------------------------------------------------------------
    app.post('/reading', async (req: Request, res: Response) => {
        const { nodeId, timestamp, moisture, humidity, temperature, pressure } = req.body;

        if (!nodeId || !timestamp || moisture === undefined || humidity === undefined ||
            temperature === undefined || pressure === undefined) {
            res.status(400).json({ error: 'Missing required fields: nodeId, timestamp, moisture, humidity, temperature, pressure' });
            return;
        }

        try {
            await contract.submitTransaction(
                'SubmitReading',
                nodeId,
                timestamp,
                moisture.toString(),
                humidity.toString(),
                temperature.toString(),
                pressure.toString()
            );
            console.log(`Reading stored: ${nodeId} @ ${timestamp}`);
            res.status(201).json({ success: true, id: `${nodeId}_${timestamp}` });
        } catch (err) {
            console.error('SubmitReading failed:', err);
            res.status(500).json({ error: String(err) });
        }
    });

    // -------------------------------------------------------------------------
    // GET /readings
    // Returns all sensor readings — used by the web dashboard.
    // -------------------------------------------------------------------------
    app.get('/readings', async (_req: Request, res: Response) => {
        try {
            const resultBytes = await contract.evaluateTransaction('GetAllReadings');
            const result = JSON.parse(utf8Decoder.decode(resultBytes));
            res.json(result);
        } catch (err) {
            console.error('GetAllReadings failed:', err);
            res.status(500).json({ error: String(err) });
        }
    });

    // -------------------------------------------------------------------------
    // GET /readings/:nodeId
    // Returns readings for a specific node — used by the web dashboard.
    // -------------------------------------------------------------------------
    app.get('/readings/:nodeId', async (req: Request, res: Response) => {
        const { nodeId } = req.params;
        try {
            const resultBytes = await contract.evaluateTransaction('GetReadingsByNode', String(nodeId));
            const result = JSON.parse(utf8Decoder.decode(resultBytes));
            res.json(result);
        } catch (err) {
            console.error('GetReadingsByNode failed:', err);
            res.status(500).json({ error: String(err) });
        }
    });

    app.listen(PORT, () => {
        console.log(`Irrigation API server running on http://localhost:${PORT}`);
        console.log(`  POST /reading          - submit sensor reading`);
        console.log(`  GET  /readings         - get all readings`);
        console.log(`  GET  /readings/:nodeId - get readings for a node`);
    });

    // Keep the gateway open for the lifetime of the server
    process.on('SIGINT', () => {
        gateway.close();
        client.close();
        process.exit(0);
    });
}

main().catch((error: unknown) => {
    console.error('FAILED to start server:', error);
    process.exitCode = 1;
});

// =============================================================================
// Fabric helpers
// =============================================================================
async function newGrpcConnection(): Promise<grpc.Client> {
    const tlsRootCert    = await fs.readFile(tlsCertPath);
    const tlsCredentials = grpc.credentials.createSsl(tlsRootCert);
    return new grpc.Client(peerEndpoint, tlsCredentials, {
        'grpc.ssl_target_name_override': peerHostAlias,
    });
}

async function newIdentity(): Promise<Identity> {
    const certPath    = await getFirstDirFileName(certDirectoryPath);
    const credentials = await fs.readFile(certPath);
    return { mspId, credentials };
}

async function getFirstDirFileName(dirPath: string): Promise<string> {
    const files = await fs.readdir(dirPath);
    const file  = files[0];
    if (!file) throw new Error(`No files in directory: ${dirPath}`);
    return path.join(dirPath, file);
}

async function newSigner(): Promise<Signer> {
    const keyPath       = await getFirstDirFileName(keyDirectoryPath);
    const privateKeyPem = await fs.readFile(keyPath);
    const privateKey    = crypto.createPrivateKey(privateKeyPem);
    return signers.newPrivateKeySigner(privateKey);
}

function envOrDefault(key: string, defaultValue: string): string {
    return process.env[key] || defaultValue;
}
