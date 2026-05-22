/*
  SPDX-License-Identifier: Apache-2.0
*/

import {Object, Property} from 'fabric-contract-api';

/* One sensor measurement taken during a visit */
@Object()
export class Reading {
    @Property()
    public Temp: number = 0;        // temperature (scaled int, e.g. /100 for °C)

    @Property()
    public Humidity: number = 0;    // % relative humidity

    @Property()
    public Moisture: number = 0;    // % soil moisture

    @Property()
    public Pressure: number = 0;    // pressure (scaled int, e.g. /100 for hPa)

    @Property()
    public MeasTime: number = 0;    // ms since node boot when measurement was taken
}

/*
 * A Visit is one mobile-node collection event at a single sensor node.
 * It holds up to 12 readings taken during that visit.
 *
 * Block key: "<NodeName>_<Timestamp>"  e.g. "garden_2026-05-21T08:32:00"
 */
@Object()
export class Visit {
    @Property()
    public docType?: string;

    @Property()
    public ID: string = '';         // "<NodeName>_<Timestamp>"

    @Property()
    public NodeName: string = '';   // human label from base-node sensor registry

    @Property()
    public Timestamp: string = '';  // ISO8601 timestamp of the visit

    @Property()
    public Readings: Reading[] = []; // 1–12 readings collected during the visit
}
