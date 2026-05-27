/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * HTTP REST server for irrigation system blockchain interface.
 *
 * POST /visit          - submit a sensor node visit (node name + timestamp + readings)
 * GET  /visits         - get all visits from the ledger
 * GET  /visits/:node   - get all visits for a specific sensor node
 * GET  /visit/:id      - get a single visit by ID
 */

// Simple terms describes the type of requests the sever can get/post.

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

const cryptoPath        = envOrDefault('CRYPTO_PATH', path.resolve(__dirname, '..', '..', '..', 'test-network', 'organizations', 'peerOrganizations', 'org1.example.com'));
const keyDirectoryPath  = envOrDefault('KEY_DIRECTORY_PATH',  path.resolve(cryptoPath, 'users', 'User1@org1.example.com', 'msp', 'keystore'));
const certDirectoryPath = envOrDefault('CERT_DIRECTORY_PATH', path.resolve(cryptoPath, 'users', 'User1@org1.example.com', 'msp', 'signcerts'));
const tlsCertPath       = envOrDefault('TLS_CERT_PATH',       path.resolve(cryptoPath, 'peers', 'peer0.org1.example.com', 'tls', 'ca.crt'));
const peerEndpoint      = envOrDefault('PEER_ENDPOINT',    'localhost:7051');
const peerHostAlias     = envOrDefault('PEER_HOST_ALIAS',  'peer0.org1.example.com');

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
    // POST /visit
    // Body: {
    //   name:     string,           // e.g. "garden"
    //   readings: Reading[]         // 1–12 items, each with:
    //     { temp, humidity, pressure, moisture, sensor_meas_time }
    //     sensor_meas_time is a Brisbane datetime string, e.g. "2026-05-26 14:32:01.456 AEST"
    // }
    // The visit-level timestamp is taken from the first reading's sensor_meas_time.
    // Called by send_http.py running on the base node host.
    // -------------------------------------------------------------------------
    app.post('/visit', async (req: Request, res: Response) => {
        const { name, readings } = req.body as { name: string; readings: Record<string, unknown>[] };

        if (!name || !Array.isArray(readings) || readings.length === 0) {
            res.status(400).json({
                error: 'Missing required fields: name (string), readings (array)',
            });
            return;
        }

        if (readings.length > 12) {
            res.status(400).json({ error: 'A visit may contain at most 12 readings' });
            return;
        }

        // Use the first reading's computed timestamp as the visit key
        const timestamp = readings[0]!.sensor_meas_time as string;

        // Map Python snake_case field names to chaincode PascalCase Reading fields
        const mappedReadings = readings.map((r) => ({
            Temp:     r.temp,
            Humidity: r.humidity,
            Pressure: r.pressure,
            Moisture: r.moisture,
            MeasTime: r.sensor_meas_time,
        }));

        try {
            await contract.submitTransaction(
                'SubmitVisit',
                name,
                timestamp,
                JSON.stringify(mappedReadings),
            );
            const id = `${name}_${timestamp}`;
            console.log(`Visit stored: ${id} (${mappedReadings.length} readings)`);
            res.status(201).json({ success: true, id });
        } catch (err) {
            console.error('SubmitVisit failed:', err);
            res.status(500).json({ error: String(err) });
        }
    });

    // -------------------------------------------------------------------------
    // GET /visits
    // Returns all visits — used by the web dashboard.
    // -------------------------------------------------------------------------
    app.get('/visits', async (_req: Request, res: Response) => {
        try {
            const resultBytes = await contract.evaluateTransaction('GetAllVisits');
            res.json(JSON.parse(utf8Decoder.decode(resultBytes)));
        } catch (err) {
            console.error('GetAllVisits failed:', err);
            res.status(500).json({ error: String(err) });
        }
    });

    // -------------------------------------------------------------------------
    // GET /visits/:nodeName
    // Returns all visits for a specific sensor node.
    // -------------------------------------------------------------------------
    app.get('/visits/:nodeName', async (req: Request, res: Response) => {
        const nodeName = req.params['nodeName'] as string;
        try {
            const resultBytes = await contract.evaluateTransaction('GetVisitsByNode', nodeName);
            res.json(JSON.parse(utf8Decoder.decode(resultBytes)));
        } catch (err) {
            console.error('GetVisitsByNode failed:', err);
            res.status(500).json({ error: String(err) });
        }
    });

    // -------------------------------------------------------------------------
    // GET /visit/:id
    // Returns a single visit by its full ID ("<nodeName>_<timestamp>").
    // -------------------------------------------------------------------------
    app.get('/visit/:id', async (req: Request, res: Response) => {
        const id = req.params['id'] as string;
        try {
            const resultBytes = await contract.evaluateTransaction('GetVisit', id);
            res.json(JSON.parse(utf8Decoder.decode(resultBytes)));
        } catch (err) {
            console.error('GetVisit failed:', err);
            res.status(500).json({ error: String(err) });
        }
    });

    app.listen(PORT, () => {
        console.log(`Irrigation API server running on http://localhost:${PORT}`);
        console.log(`  POST /visit            - submit a sensor node visit`);
        console.log(`  GET  /visits           - get all visits`);
        console.log(`  GET  /visits/:nodeName - get visits for a node`);
        console.log(`  GET  /visit/:id        - get a single visit by ID`);
    });

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
