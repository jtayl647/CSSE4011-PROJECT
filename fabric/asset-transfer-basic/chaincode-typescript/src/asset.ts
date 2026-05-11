/*
  SPDX-License-Identifier: Apache-2.0
*/

import {Object, Property} from 'fabric-contract-api';

@Object()
export class SensorReading {
    @Property()
    public docType?: string;

    @Property()
    public ID: string = '';          // e.g. "node1_2026-05-11T08:32:00"

    @Property()
    public NodeID: string = '';      // e.g. "node1"

    @Property()
    public Timestamp: string = '';   // ISO8601 e.g. "2026-05-11T08:32:00"

    @Property()
    public Moisture: number = 0;     // % soil moisture

    @Property()
    public Humidity: number = 0;     // % relative humidity

    @Property()
    public Temperature: number = 0;  // degrees C

    @Property()
    public Pressure: number = 0;     // hPa
}
