#!/usr/bin/env python3
"""Decode OpenYAMM 01.06/legacy binary captures; Python 3 standard library only."""
import argparse,json,struct
from pathlib import Path
SCHEMA = {'magic': 3474298754897628493, 'bytes': 66048, 'header_words': 64, 'row_words': 128, 'fields': ['frame', 'bgfxFrame', 'draws', 'dead', 'outer.0', 'outer.1', 'outer.2', 'outer.3', 'outer.4', 'game.0', 'game.1', 'game.2', 'game.3', 'game.4', 'game.5', 'game.6', 'child.0', 'child.1', 'child.2', 'child.3', 'child.4', 'child.5', 'child.6', 'child.7', 'child.8', 'child.9', 'child.10', 'child.11', 'child.12', 'child.13', 'child.14', 'child.15', 'child.16', 'child.17', 'child.18', 'child.19', 'child.20', 'child.21', 'child.22', 'child.23', 'child.24', 'child.25', 'child.26', 'child.27', 'child.28', 'child.29', 'child.30', 'child.31', 'child.32', 'child.33', 'child.34', 'child.35', 'child.36', 'child.37', 'child.38', 'child.39', 'renderNs', 'submitWaitNs', 'renderWaitNs', 'simulationHz', 'steps', 'actors', 'activeSum', 'backgroundSum', 'activatedSectors', 'projectiles', 'impacts', 'portalBuilds', 'portalNodes', 'textures', 'requests', 'uploads', 'resident', 'evictions', 'deferred', 'recoveryCalls', 'recoveryCandidates', 'recoveryCatalogue', 'recoveryFallbacks', 'movementCalls', 'physicsCalls', 'hudTextures', 'hudFonts', 'footSector', 'eyeSector', 'partyX', 'partyY', 'partyZ', 'supportFace', 'minimapRebuilds', 'minimapLines', 'revealUpdates', 'invalidSectorMasks', 'pathPending', 'pathRunning', 'staticHits', 'staticMisses', 'bloodDraws', 'bloodCulled', 'bloodUploads', 'bloodVertices', 'actorPrepared', 'actorDrawn', 'geometryDraws', 'geometryLights', 'fxLights', 'snapshotItems', 'fineSample', 'directChecks', 'directCacheHits', 'longChecksDeferred', 'targetPolicyBuilds', 'overrideEvaluations', 'factResets', 'outTerrainDraws', 'outBuildingDraws', 'outHighlightDraws', 'outActorDraws', 'outDecorationDraws','uiAsyncRequests','uiAsyncInputBlocked','uiSyncLoads','uiSyncNs','outColliderBuilds','outColliderVisits','outColliderUpdates','outColliderMembershipRebuilds','outColliderAttackInvalidations'], 'float_fields': ['partyX', 'partyY', 'partyZ']}

def checksum(data):
    value=2166136261
    for byte in data: value=((value^byte)*16777619)&0xffffffff
    return value

def worker_snapshot(raw):
    words=struct.unpack_from('<192Q',raw,512)
    stream_names=['instance','frame','queued','readyBytes','hits','decoded','failures','consumed','expired','queueFull','readUs','readyHead','setupUs','readyWaitUs','preparedUs','catalogueHits','retries','writeUs','writeFailures','bc3Hits']
    fields=['id','role','queryResult','status','waitType','waitId','priority','cpu','rawRunClocks','stage','stageAgeUs','detail','mutexAddress','waitCaller','mutexOwnerId','lastFailure']
    return dict(tickUs=words[0],frame=words[1],frameAgeUs=words[2],context=words[3],sector=words[4],step=words[5],streamValid=bool(words[6]),
        stream=dict(zip(stream_names,words[8:28])) if words[6] else None,
        registered=words[28],omitted=words[29],threads=[dict(zip(fields,words[32+i*16:48+i*16])) for i in range(min(words[7],10))])

def decode(data):
    size=SCHEMA['bytes'];magic=struct.pack('<Q',SCHEMA['magic']);house_magic=struct.pack('<Q',0x3037304859414d4d);watch_magic=struct.pack('<Q',0x3138305759414d4d);records=[];errors=[];offset=0
    while offset<len(data):
        if data[offset:offset+8] not in (magic,house_magic,watch_magic):
            candidates=[n for n in (data.find(magic,offset+1),data.find(house_magic,offset+1),data.find(watch_magic,offset+1)) if n>=0]
            found=min(candidates) if candidates else -1
            errors.append({'offset':offset,'error':'invalid magic or gap'})
            if found<0:break
            offset=found
        if len(data)-offset<512:
            errors.append({'offset':offset,'error':'incomplete header'});break
        size=struct.unpack_from('<Q',data,offset+16)[0]
        if size not in (2048,61952,66048):
            errors.append({'offset':offset,'error':'invalid record size'});offset+=8;continue
        if len(data)-offset<size:
            errors.append({'offset':offset,'error':'incomplete final record','available':len(data)-offset});break
        raw=bytearray(data[offset:offset+size]);header=struct.unpack_from('<64Q',raw)
        expected=header[63];struct.pack_into('<Q',raw,63*8,0)
        if raw[:8]==watch_magic:
            if size!=2048 or header[1] not in (1,2,3) or checksum(raw)!=expected:
                errors.append({'offset':offset,'error':'heartbeat schema/checksum failure'});offset+=size;continue
            record=dict(type='heartbeat',offset=offset,settings_id=header[11],workers=worker_snapshot(raw))
            if header[1]>=2:
                record['lock_tracking']=bool(header[12])
                record['health_words']=list(header[16:39])
                record['activity']=dict(zip(['partyDamage','damagePortrait','newUiAsset','newSoundDecode','cacheWriteQueued','cacheWriteSkipped','cacheWriteDone','cacheWriteFailed','actorRayHit','actorRayMiss'],header[40:50]))
                record['native_locks']=dict(zip(['maxHoldUs','maxWork','maxOwner','maxCaller','maxWaitUs','holdsGe1ms','currentMaxHoldUs','currentWork','currentOwner','tableOverflow'],header[50:60]))
                record['heap_current_hold_us']=header[60]
            if header[1]>=3:
                record['heap_work_raw']=list(header[3:7])
                record['heap_wait_total_us_mod32']=header[7]
                record['heap_waits_ge_1ms']=header[8]
                record['path_yields']=header[9]
                record['path_max_poll_gap_us']=header[10]
            records.append(record)
            offset+=size;continue
        is_house=raw[:8]==house_magic
        if header[1] not in ((1,2) if is_house else ((1,) if size==61952 else (2,3,4,5,6,7,8,9,10,11,12,13,14,15))) or header[2]!=size or header[3]!=(46 if is_house else (120 if size==61952 else (224 if header[1]>=14 else 208 if header[1]>=11 else 192 if header[1]>=7 else (144 if header[1]>=5 else 128)))) or header[4]>(181 if is_house else (35 if header[1]>=14 else 38 if header[1]>=11 else 41 if header[1]>=7 else (55 if header[1]==6 else (56 if header[1]==5 else 64)))) or checksum(raw)!=expected:
            errors.append({'offset':offset,'error':'schema/count/checksum failure'});offset+=8;continue
        if is_house:
            fields=['elapsedMs','loops','presented','emptyVideo','emptyAudio','minimumVideo','minimumAudioFrames','minimumQueuedBytes']
            record=dict(type='house',settings_id=header[11] if header[1]>=2 else 0,offset=offset,paused_presentation=bool(header[5]),start_ns=header[6],clip=raw[128:256].split(b'\0',1)[0].decode('utf-8','replace'),rows=[])
            for i in range(header[4]):
                values=struct.unpack_from('<46I',raw,512+i*46*4)
                row=dict(zip(fields,values[:8]));row['sections']=list(values[8:38]);row['graphics']=list(values[38:46]);record['rows'].append(row)
            records.append(record);offset+=size;continue
        record=dict(settings_id=header[11] if header[1]>=4 else 0,offset=offset,scene=header[5],house=header[6],completed=header[7],dropped=header[8],previous_write_ms=header[9],tick_ns=header[10],map=raw[128:256].split(b'\0',1)[0].decode('utf-8','replace'),reason=raw[256:320].split(b'\0',1)[0].decode('utf-8','replace'),rows=[])
        if header[1] in (2,3,4,5,6,7,8,9,10,11,12,13,14,15):
            health_names=['heapAcquires','heapMaxHoldUs','heapMaxOwner','heapMaxCaller','heapMaxWaitUs','heapHoldsGe1ms','sbrksDuringMaxHold','sbrkCalls','sbrkGrow','sbrkTrim','sbrkUs','sbrkMaxUs','sbrkFailures','watchdogOpenError','watchdogWriteError','heapCurrentOwner','heapCurrentCaller','healthTickUs']
            record['cpu_probe_wall_us_run_ticks'] = list(header[12:16]) if header[1]>=5 else []
            record['health']=dict(zip(health_names,header[40:58]))
            if header[1]>=3: record['health'].update(zip(['watchdogStartup','watchdogPolls','mutexMaxWaitUs','mutexMaxThread','mutexMaxCaller'],header[58:63]))
        if header[1]>=6: record['workers']=worker_snapshot(raw)
        for i in range(header[4]):
            values=struct.unpack_from('<'+str(header[3])+'Q',raw,((256 if header[1]>=6 else 64)+i*header[3])*8);fields=SCHEMA['fields']
            if header[1]>=3: fields=fields[:56]+['child.'+str(j) for j in range(40,69 if header[1]>=8 else (63 if header[1]>=7 else (56 if header[1]>=5 else (45 if header[1]==4 else 43))))]+fields[56:]
            if header[1]>=7: fields=fields+['waypointChecks', 'waypointCacheHits', 'waypointLongDeferred', 'physicsWorkCalls', 'floorQueries', 'sectorQueries', 'collisionQueries', 'pathQueueAcquires', 'sectorCatalogue', 'sectorCandidates', 'applyCpuTicks', 'applyCpuProbes', 'applyCpuProbeFailures']
            if header[1]>=8: fields=fields+['floorFaceCatalogue','floorFaceCandidates','combatTargetCatalogue','combatTargetEligible','selectedSameSector','selectedOtherSector','selectedVertical256']
            if header[1]>=9: fields=fields+['floorProjectionChecks','floorProjectionHits','pointSectorChecks','pointSectorHits','combatQueryCalls','combatQueryHits']
            if header[1]>=10: fields=fields+['ceilingFaceCatalogue', 'ceilingFaceCandidates', 'sweepFaceCatalogue', 'sweepFaceCandidates', 'pathPrefixPacked', 'pathCompleted', 'pathNodes', 'pathMaxDeliveryMs', 'pathRefreshed']
            if header[1]>=11: fields=fields+['terrainPrepareNs', 'terrainUploadNs', 'terrainCacheBytes', 'terrainCachePeak', 'terrainCacheHits', 'terrainCacheMisses', 'terrainCacheEvictions', 'terrainSlots', 'terrainVisibleSlots', 'terrainUpdatedSlots', 'terrainUploadCalls', 'terrainUploadBytes']
            if header[1]>=12: fields=fields+['supportMemoPacked','terrainFastDraws']
            if header[1]>=13: fields=fields+['worldResolutionMode']
            if header[1]>=14: fields+=['resolutionTransitions','pathPartial','pathSuccess','pathBudget','pathRefinement','pathMaxQueueMs','pathMaxSearchMs']
            if header[1]>=15: fields+=['waypointCertificateHits','pathPartialRejected','pathShortcutsDeferred']
            row=dict(zip(fields,values))
            if header[1]>=12:
                row['supportMemoChecks']=row['supportMemoPacked']&0xffffffff
                row['supportMemoHits']=row['supportMemoPacked']>>32
            if header[1]>=10:
                row["pathPrefixUsed"]=row["pathPrefixPacked"]&0xffffffff
                row["pathPrefixBlocked"]=row["pathPrefixPacked"]>>32
                row["pathCancelled"]=row["pathCompleted"]>>32
                row["pathCompleted"]&=0xffffffff
                row["pathOldestPendingMs"]=row["pathMaxDeliveryMs"]>>32
                row["pathMaxDeliveryMs"]&=0xffffffff
            for name in SCHEMA['float_fields']: row[name]=struct.unpack('<f',struct.pack('<I',row[name]&0xffffffff))[0]
            for name in ['footSector','eyeSector']:
                if row[name]>=1<<63:row[name]-=1<<64
            record['rows'].append(row)
        records.append(record);offset+=size
    return dict(records=records,errors=errors)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('-o','--output',type=Path);a=p.parse_args()
    result=decode(a.capture.read_bytes());dest=a.output or a.capture.with_suffix('.decoded.json')
    dest.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(f"Decoded {len(result['records'])} snapshots; {len(result['errors'])} incomplete/corrupt sections. Output: {dest}")
