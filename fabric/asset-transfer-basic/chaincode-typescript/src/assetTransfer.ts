/*
 * SPDX-License-Identifier: Apache-2.0
 */
import {Context, Contract, Info, Returns, Transaction} from 'fabric-contract-api';
import stringify from 'json-stringify-deterministic';
import sortKeysRecursive from 'sort-keys-recursive';
import {Visit, Reading} from './asset';

// Smart contract that runs on the blockchain!
// 
// How a visit should look.

@Info({title: 'IrrigationData', description: 'Smart contract for storing sensor node visits'})
export class IrrigationDataContract extends Contract {

    /*
     * SubmitVisit — store one mobile-node visit on the ledger.
     *
     * @param nodeName   Human label for the sensor node (e.g. "garden")
     * @param timestamp  ISO8601 string for when the visit occurred
     * @param readings   JSON array of Reading objects (max 12)
     *                   e.g. '[{"Temp":2310,"Humidity":55,"Moisture":42,"Pressure":101200,"MeasTime":1500}]'
     */
    @Transaction()
    public async SubmitVisit(
        ctx: Context,
        nodeName: string,
        timestamp: string,
        readings: string,
    ): Promise<void> {
        const id = `${nodeName}_${timestamp}`;

        let parsedReadings: Reading[];
        try {
            parsedReadings = JSON.parse(readings) as Reading[];
        } catch {
            throw new Error(`Invalid readings JSON: ${readings}`);
        }

        if (parsedReadings.length === 0 || parsedReadings.length > 12) {
            throw new Error(`Readings must contain 1–12 entries, got ${parsedReadings.length}`);
        }

        const visit: Visit = {
            docType:   'visit',
            ID:        id,
            NodeName:  nodeName,
            Timestamp: timestamp,
            Readings:  parsedReadings,
        };

        await ctx.stub.putState(id, Buffer.from(stringify(sortKeysRecursive(visit))));
        console.info(`Visit ${id} stored (${parsedReadings.length} readings)`);
    }

    /*
     * GetVisit — return a single visit by its ID.
     */
    @Transaction(false)
    @Returns('string')
    public async GetVisit(ctx: Context, id: string): Promise<string> {
        const data = await ctx.stub.getState(id);
        if (data.length === 0) {
            throw new Error(`Visit ${id} does not exist`);
        }
        return data.toString();
    }

    /*
     * GetAllVisits — return every visit on the ledger.
     */
    @Transaction(false)
    @Returns('string')
    public async GetAllVisits(ctx: Context): Promise<string> {
        const results: Visit[] = [];
        const iterator = await ctx.stub.getStateByRange('', '');
        let result = await iterator.next();
        while (!result.done) {
            const str = Buffer.from(result.value.value).toString('utf8');
            try {
                results.push(JSON.parse(str) as Visit);
            } catch {
                /* skip malformed entries */
            }
            result = await iterator.next();
        }
        return JSON.stringify(results);
    }

    /*
     * GetVisitsByNode — return all visits for a specific sensor node.
     * Range query relies on the "<NodeName>_<Timestamp>" key format.
     */
    @Transaction(false)
    @Returns('string')
    public async GetVisitsByNode(ctx: Context, nodeName: string): Promise<string> {
        const results: Visit[] = [];
        /* '~' sorts after all printable ASCII so this covers the full node range */
        const iterator = await ctx.stub.getStateByRange(`${nodeName}_`, `${nodeName}_~`);
        let result = await iterator.next();
        while (!result.done) {
            const str = Buffer.from(result.value.value).toString('utf8');
            try {
                results.push(JSON.parse(str) as Visit);
            } catch {
                /* skip malformed entries */
            }
            result = await iterator.next();
        }
        return JSON.stringify(results);
    }
}
