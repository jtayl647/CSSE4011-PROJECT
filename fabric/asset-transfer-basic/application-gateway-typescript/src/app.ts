/*
 * SPDX-License-Identifier: Apache-2.0
 */

import * as grpc from '@grpc/grpc-js';
import { connect, Contract, hash, Identity, Signer, signers } from '@hyperledger/fabric-gateway';
import * as crypto from 'crypto';
import { promises as fs } from 'fs';
import * as path from 'path';
import { TextDecoder } from 'util';

const channelName = envOrDefault('CHANNEL_NAME', 'mychannel');
const chaincodeName = envOrDefault('CHAINCODE_NAME', 'irrigation');
const mspId = envOrDefault('MSP_ID', 'Org1MSP');

const cryptoPath = envOrDefault('CRYPTO_PATH', path.resolve(__dirname, '..', '..', '..', 'test-network', 'organizations', 'peerOrganizations', 'org1.example.com'));
const keyDirectoryPath = envOrDefault('KEY_DIRECTORY_PATH', path.resolve(cryptoPath, 'users', 'User1@org1.example.com', 'msp', 'keystore'));
const certDirectoryPath = envOrDefault('CERT_DIRECTORY_PATH', path.resolve(cryptoPath, 'users', 'User1@org1.example.com', 'msp', 'signcerts'));
const tlsCertPath = envOrDefault('TLS_CERT_PATH', path.resolve(cryptoPath, 'peers', 'peer0.org1.example.com', 'tls', 'ca.crt'));
const peerEndpoint = envOrDefault('PEER_ENDPOINT', 'localhost:7051');
const peerHostAlias = envOrDefault('PEER_HOST_ALIAS', 'peer0.org1.example.com');

const utf8Decoder = new TextDecoder();

async function main(): Promise<void> {
    const client = await newGrpcConnection();

    const gateway = connect({
        client,
        identity: await newIdentity(),
        signer: await newSigner(),
        hash: hash.sha256,
        evaluateOptions: () => ({ deadline: Date.now() + 5000 }),
        endorseOptions: () => ({ deadline: Date.now() + 15000 }),
        submitOptions: () => ({ deadline: Date.now() + 5000 }),
        commitStatusOptions: () => ({ deadline: Date.now() + 60000 }),
    });

    try {
        const network = gateway.getNetwork(channelName);
        const contract = network.getContract(chaincodeName);

        // Submit a test sensor reading
        await submitReading(contract, 'node1', new Date().toISOString(), 45, 62, 24, 1013);

        // Get all readings
        await getAllReadings(contract);

        // Get readings for node1 only
        await getReadingsByNode(contract, 'node1');

    } finally {
        gateway.close();
        client.close();
    }
}

main().catch((error: unknown) => {
    console.error('FAILED to run application:', error);
    process.exitCode = 1;
});

// Submit a sensor reading to the blockchain
export async function submitReading(
    contract: Contract,
    nodeId: string,
    timestamp: string,
    moisture: number,
    humidity: number,
    temperature: number,
    pressure: number
): Promise<void> {
    console.log(`\n--> Submitting reading for ${nodeId} at ${timestamp}`);
    await contract.submitTransaction(
        'SubmitReading',
        nodeId,
        timestamp,
        moisture.toString(),
        humidity.toString(),
        temperature.toString(),
        pressure.toString()
    );
    console.log('*** Reading committed to blockchain');
}

// Get all readings from the blockchain
export async function getAllReadings(contract: Contract): Promise<void> {
    console.log('\n--> Getting all sensor readings');
    const resultBytes = await contract.evaluateTransaction('GetAllReadings');
    const result = JSON.parse(utf8Decoder.decode(resultBytes));
    console.log('*** All readings:', JSON.stringify(result, null, 2));
}

// Get all readings for a specific node
export async function getReadingsByNode(contract: Contract, nodeId: string): Promise<void> {
    console.log(`\n--> Getting readings for ${nodeId}`);
    const resultBytes = await contract.evaluateTransaction('GetReadingsByNode', nodeId);
    const result = JSON.parse(utf8Decoder.decode(resultBytes));
    console.log(`*** Readings for ${nodeId}:`, JSON.stringify(result, null, 2));
}

async function newGrpcConnection(): Promise<grpc.Client> {
    const tlsRootCert = await fs.readFile(tlsCertPath);
    const tlsCredentials = grpc.credentials.createSsl(tlsRootCert);
    return new grpc.Client(peerEndpoint, tlsCredentials, {
        'grpc.ssl_target_name_override': peerHostAlias,
    });
}

async function newIdentity(): Promise<Identity> {
    const certPath = await getFirstDirFileName(certDirectoryPath);
    const credentials = await fs.readFile(certPath);
    return { mspId, credentials };
}

async function getFirstDirFileName(dirPath: string): Promise<string> {
    const files = await fs.readdir(dirPath);
    const file = files[0];
    if (!file) throw new Error(`No files in directory: ${dirPath}`);
    return path.join(dirPath, file);
}

async function newSigner(): Promise<Signer> {
    const keyPath = await getFirstDirFileName(keyDirectoryPath);
    const privateKeyPem = await fs.readFile(keyPath);
    const privateKey = crypto.createPrivateKey(privateKeyPem);
    return signers.newPrivateKeySigner(privateKey);
}

function envOrDefault(key: string, defaultValue: string): string {
    return process.env[key] || defaultValue;
}
