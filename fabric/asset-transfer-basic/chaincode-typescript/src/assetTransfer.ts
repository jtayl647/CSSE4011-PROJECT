/*
 * SPDX-License-Identifier: Apache-2.0
 */
import {Context, Contract, Info, Returns, Transaction} from 'fabric-contract-api';
import stringify from 'json-stringify-deterministic';
import sortKeysRecursive from 'sort-keys-recursive';
import {SensorReading} from './asset';

@Info({title: 'IrrigationData', description: 'Smart contract for storing irrigation sensor readings'})
export class IrrigationDataContract extends Contract {

    // SubmitReading stores a new sensor reading on the ledger
    @Transaction()
    public async SubmitReading(
        ctx: Context,
        nodeId: string,
        timestamp: string,
        moisture: number,
        humidity: number,
        temperature: number,
        pressure: number
    ): Promise<void> {
        const id = `${nodeId}_${timestamp}`;

        const reading: SensorReading = {
            docType: 'sensorReading',
            ID: id,
            NodeID: nodeId,
            Timestamp: timestamp,
            Moisture: moisture,
            Humidity: humidity,
            Temperature: temperature,
            Pressure: pressure,
        };

        await ctx.stub.putState(id, Buffer.from(stringify(sortKeysRecursive(reading))));
        console.info(`Reading ${id} stored`);
    }

    // GetReading returns a single reading by its ID
    @Transaction(false)
    @Returns('string')
    public async GetReading(ctx: Context, id: string): Promise<string> {
        const data = await ctx.stub.getState(id);
        if (data.length === 0) {
            throw new Error(`Reading ${id} does not exist`);
        }
        return data.toString();
    }

    // GetAllReadings returns all sensor readings on the ledger
    @Transaction(false)
    @Returns('string')
    public async GetAllReadings(ctx: Context): Promise<string> {
        const allResults = [];
        const iterator = await ctx.stub.getStateByRange('', '');
        let result = await iterator.next();
        while (!result.done) {
            const strValue = Buffer.from(result.value.value.toString()).toString('utf8');
            let record;
            try {
                record = JSON.parse(strValue) as SensorReading;
            } catch (err) {
                record = strValue;
            }
            allResults.push(record);
            result = await iterator.next();
        }
        return JSON.stringify(allResults);
    }

    // GetReadingsByNode returns all readings for a specific sensor node
    @Transaction(false)
    @Returns('string')
    public async GetReadingsByNode(ctx: Context, nodeId: string): Promise<string> {
        const allResults = [];
        const iterator = await ctx.stub.getStateByRange(`${nodeId}_`, `${nodeId}_~`);
        let result = await iterator.next();
        while (!result.done) {
            const strValue = Buffer.from(result.value.value.toString()).toString('utf8');
            let record;
            try {
                record = JSON.parse(strValue) as SensorReading;
            } catch (err) {
                record = strValue;
            }
            allResults.push(record);
            result = await iterator.next();
        }
        return JSON.stringify(allResults);
    }
}
