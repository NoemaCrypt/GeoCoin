#!/usr/bin/env python3
import argparse
import base64
import hashlib
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

CREDS = {
    'admin': ('admin', 'admin-pass'),
    'ro': ('ro', 'ro-pass'),
    'miner': ('miner', 'miner-pass'),
}

CONTROLLED_ERRORS = {
    -32600, -32602, -3, -1, -4, -5, -6, -8, -9, -10,
    -20, -22, -23, -24, -25, -26, -27, -29,
}

ALL_REGISTERED = [
    'addnode', 'analyzegeometry', 'burnnft', 'clearbans', 'createaccountingcheckpointproofv1',
    'createaccountingsidechainpayload', 'createbatchsidechaincheckpointpayload',
    'createcheckpointproofv2', 'createdelegationproof', 'createdelegationproofdraft',
    'createhierarchicalspenddraft', 'createnft', 'createnftburnpayload',
    'createnftcreatepayload', 'createnfttransferpayload', 'createrawtransaction',
    'createrevokedelegation', 'createrevokedelegationdraft', 'createsidechaincheckpointpayload',
    'createsidechaincommitteeupdatepayload', 'createsidechaindecommissionpayload',
    'createsidechaingenesis', 'createsidechainissuepayload', 'createsidechainlockpayload',
    'createsidechainmintpayload', 'createsidechainredeempayload',
    'createsidechainslashpayload', 'createsidechainspendpayload',
    'createsidechaintopuppayload', 'createsidechainunlockpayload', 'createwallet', 'decoderawtransaction',
    'disconnectnode', 'dumpprivkey', 'finalizebatchsidechaincheckpoint',
    'finalizebatchsidechaincheckpointandsubmit', 'finalizedelegationproof',
    'finalizehierarchicalspend', 'finalizeaccountingsidechain',
    'finalizeaccountingsidechainandsubmit', 'finalizenftburn', 'finalizenftcreate',
    'finalizenfttransfer', 'finalizesidechaincheckpoint',
    'finalizesidechaincommitteeupdate', 'finalizesidechaincommitteeupdateandsubmit',
    'finalizesidechaindecommission', 'finalizesidechaindecommissionandsubmit',
    'finalizesidechainissue', 'finalizesidechainissueandsubmit',
    'finalizesidechainlock', 'finalizesidechainmint', 'finalizesidechainmintandsubmit',
    'finalizesidechainredeem', 'finalizesidechainredeemandsubmit',
    'finalizesidechainslash', 'finalizesidechainslashandsubmit', 'finalizesidechaintopup',
    'finalizesidechainspend', 'finalizesidechainunlock', 'getaddednodeinfo',
    'getaddressbalance', 'getaddressesbyaccount', 'getaddresshistory',
    'getaddresstxids', 'getaddressutxos', 'getavailableblocks', 'getbalance',
    'getblock', 'getblockavailability', 'getblockbyheight', 'getblockchaininfo',
    'getblockchildren', 'getblockcount', 'getblockhash', 'getblockrejectstats',
    'getblocktemplate', 'getblocktree', 'getconnectioncount',
    'getconnectiondiagnostics', 'getdbstats', 'getdelegationproof', 'getfalconinfo',
    'getfeerate', 'getfundbalance', 'gethealth', 'getinfo', 'getinterestpool',
    'getmempoolentry', 'getmempoolinfo', 'getmetrics', 'getmininginfo',
    'getnetworkhashps', 'getnewaddress', 'getnft', 'getnftgeojson', 'getnftshistory', 'getnodeinfo',
    'getp2ppolicystats', 'getpeerinfo', 'getprunedrange', 'getpruninginfo',
    'getrawmempool', 'getrawtransaction', 'getrpcinfo', 'getsidechaingenesis',
    'getstorageinfo', 'getsyncstatus', 'gettxout', 'getutxos',
    'getwalletaddressinfo', 'getwitnesspubkeystats', 'help', 'importprivkey', 'listaddressbalances',
    'listbans', 'listblockgeography', 'listnftsbyowner', 'listtransactions', 'listunspent',
    'powdeterminism', 'prunenow', 'registermultisig', 'senddelegationpackage',
    'sendmultisigpackage', 'sendrawtransaction', 'sendrevokepackage',
    'sendtoaddress', 'setban', 'setgenerate', 'signanglecommitment', 'signmessage',
    'signrawtransactionwithwallet', 'stop', 'submitblock', 'switchtotip',
    'testmempoolaccept', 'transfernft', 'unprune', 'validateaddress',
    'verifyrawtransaction', 'verifysignature',
]


def scan_registered_methods_from_source():
    source_dir = Path(__file__).resolve().parents[1] / 'src' / 'geocoin' / 'rpc'
    if not source_dir.exists():
        return []
    names = set()
    for path in source_dir.glob('*.cpp'):
        try:
            text = path.read_text(encoding='utf-8', errors='ignore')
        except OSError:
            continue
        names.update(re.findall(r'\.name\s*=\s*"([^"]+)"', text))
    return sorted(names)


def hash_hex(seed):
    return hashlib.sha256(('geocoin-rpc-smoke-%d' % seed).encode('ascii')).hexdigest()


def tx_input(seed, vout=0):
    return {
        'txid': hash_hex(seed),
        'vout': vout,
        'sequence': 0,
        'script_sig_hex': '',
    }


def tx_output(address, value):
    return {
        'address': address,
        'value': value,
    }

class RpcTransportError(Exception):
    pass

class RpcSmoke:
    def __init__(self, args):
        self.args = args
        self.rpc_url = 'http://127.0.0.1:%d/' % args.rpc_port
        self.results = []
        self.seen = set()
        self.ctx = {}
        self.source_methods = scan_registered_methods_from_source()
        # Keep a reviewed manifest independent from the source scan. A command
        # cannot silently disappear from both source and live registry, and a
        # new command must receive an explicit smoke scenario before landing.
        self.expected_methods = sorted(ALL_REGISTERED)
        self.process = None
        self.log_file = None
        self.data_dir_created = False

    def rpc(self, method, params=None, role='admin', timeout=10):
        if params is None:
            params = []
        user, password = CREDS[role]
        payload = {'jsonrpc': '2.0', 'id': method, 'method': method, 'params': params}
        token = base64.b64encode(('%s:%s' % (user, password)).encode()).decode()
        req = urllib.request.Request(
            self.rpc_url,
            data=json.dumps(payload, separators=(',', ':')).encode(),
            headers={'Content-Type': 'application/json', 'Authorization': 'Basic ' + token},
            method='POST',
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                body = resp.read().decode()
                if resp.status != 200:
                    raise RpcTransportError('HTTP %d: %s' % (resp.status, body[:300]))
                return json.loads(body)
        except urllib.error.HTTPError as exc:
            body = exc.read().decode(errors='replace')
            raise RpcTransportError('HTTP %d: %s' % (exc.code, body[:300]))
        except json.JSONDecodeError as exc:
            raise RpcTransportError('Bad JSON: %s' % exc)
        except Exception as exc:
            if isinstance(exc, RpcTransportError):
                raise
            raise RpcTransportError(str(exc))

    @staticmethod
    def errcode(response):
        if isinstance(response, dict) and response.get('error'):
            return response['error'].get('code')
        return None

    @staticmethod
    def result_of(response):
        if isinstance(response, dict) and response.get('error') is None:
            return response.get('result')
        return None

    def record(self, method, params=None, role='admin', expect='success', allowed=None, note=''):
        if allowed is None:
            allowed = CONTROLLED_ERRORS
        self.seen.add(method)
        entry = {'method': method, 'role': role, 'expect': expect, 'note': note}
        try:
            response = self.rpc(method, params or [], role=role)
            entry['response'] = response
            code = self.errcode(response)
            if expect == 'success':
                ok = code is None
            elif expect == 'error':
                ok = code in allowed
            elif expect == 'success_or_error':
                ok = (code is None) or (code in allowed)
            else:
                raise ValueError(expect)
            if code in (-32601, -32603):
                ok = False
            entry['ok'] = ok
            entry['code'] = code
            if not ok:
                entry['why'] = 'unexpected code=%r response=%r' % (code, response)
            self.results.append(entry)
            return response
        except Exception as exc:
            entry['ok'] = False
            entry['transport_error'] = str(exc)
            self.results.append(entry)
            return {'error': {'code': 'transport', 'message': str(exc)}, 'result': None}

    def record_check(self, method, ok, note='', details=None):
        entry = {'method': method, 'role': 'local-check', 'expect': 'true', 'ok': bool(ok), 'note': note}
        if details is not None:
            entry['details'] = details
        if not ok:
            entry['why'] = note or 'check failed'
        self.results.append(entry)

    def start_node(self):
        data_dir = Path(self.args.data_dir)
        if data_dir.exists() and self.args.clean_data_dir:
            shutil.rmtree(data_dir)
        data_dir.mkdir(parents=True, exist_ok=True)
        self.log_file = open(data_dir / 'rpc-smoke-node.log', 'w', encoding='utf-8')
        cmd = [
            self.args.node_bin,
            '--data-dir', str(data_dir),
            '--network', self.args.network,
            '--archive',
            '--p2p-port', str(self.args.p2p_port),
            '--rpc',
            '--rpc-port', str(self.args.rpc_port),
            '--rpc-user', CREDS['admin'][0],
            '--rpc-pass', CREDS['admin'][1],
            '--rpc-readonly-user', CREDS['ro'][0],
            '--rpc-readonly-pass', CREDS['ro'][1],
            '--rpc-miner-user', CREDS['miner'][0],
            '--rpc-miner-pass', CREDS['miner'][1],
            '--status-interval', '10',
        ]
        for seed in self.args.addnode:
            cmd.extend(['--addnode', seed])
        popen_kwargs = {
            'stdout': self.log_file,
            'stderr': subprocess.STDOUT,
            'stdin': subprocess.DEVNULL,
            'text': True,
        }
        env = os.environ.copy()
        env['GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW'] = '0'
        env.setdefault('GEOCOIN_ALLOW_PLAINTEXT_WALLET', '1')
        popen_kwargs['env'] = env
        if os.name != 'nt':
            popen_kwargs['preexec_fn'] = os.setsid
        self.process = subprocess.Popen(cmd, **popen_kwargs)

    def wait_rpc_and_sync(self):
        deadline = time.monotonic() + self.args.sync_timeout
        last = None
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError('node exited early with rc=%s' % self.process.returncode)
            try:
                info = self.result_of(self.rpc('getblockchaininfo', [], role='ro', timeout=5))
                peers = self.result_of(self.rpc('getconnectioncount', [], role='ro', timeout=5))
                if isinstance(info, dict):
                    last = (info, peers)
                    blocks = int(info.get('blocks') or 0)
                    headers = int(info.get('headers') or 0)
                    syncing = bool(info.get('syncing'))
                    if blocks >= self.args.min_height and blocks == headers and not syncing:
                        self.ctx['sync'] = {
                            'blocks': blocks,
                            'headers': headers,
                            'syncing': syncing,
                            'peers': peers,
                            'bestblockhash': info.get('bestblockhash'),
                        }
                        return
            except Exception:
                pass
            time.sleep(2)
        raise RuntimeError('sync timeout, last=%r' % (last,))

    def record_owner_signed_sidechain_flow(self, create_method, finalize_method, submit_method, request, signature_hex):
        created = self.result_of(self.record(create_method, [request], 'admin', 'success')) or {}
        tx_hex = created.get('tx_hex') if isinstance(created, dict) else ''
        if tx_hex:
            self.record(finalize_method, [tx_hex, signature_hex], 'admin', 'success')
            if submit_method:
                self.record(submit_method, [tx_hex, signature_hex], 'admin', 'error', allowed={-25, -26, -27})
        else:
            self.record(finalize_method, ['00', signature_hex], 'admin', 'error')
            if submit_method:
                self.record(submit_method, ['00', signature_hex], 'admin', 'error')

    def record_decommission_sidechain_flow(self, create_method, finalize_method, submit_method, request, owner_signature_hex, committee_signatures):
        created = self.result_of(self.record(create_method, [request], 'admin', 'success')) or {}
        tx_hex = created.get('tx_hex') if isinstance(created, dict) else ''
        if tx_hex:
            self.record(finalize_method, [tx_hex, owner_signature_hex, committee_signatures], 'admin', 'success')
            if submit_method:
                self.record(submit_method, [tx_hex, owner_signature_hex, committee_signatures], 'admin', 'error', allowed={-25, -26, -27})
        else:
            self.record(finalize_method, ['00', owner_signature_hex, committee_signatures], 'admin', 'error')
            if submit_method:
                self.record(submit_method, ['00', owner_signature_hex, committee_signatures], 'admin', 'error')

    def record_committee_signed_sidechain_flow(self, create_method, finalize_method, submit_method, request, signatures):
        created = self.result_of(self.record(create_method, [request], 'admin', 'success')) or {}
        tx_hex = created.get('tx_hex') if isinstance(created, dict) else ''
        if tx_hex:
            self.record(finalize_method, [tx_hex, signatures], 'admin', 'success')
            if submit_method:
                self.record(submit_method, [tx_hex, signatures], 'admin', 'error', allowed={-25, -26, -27})
        else:
            self.record(finalize_method, ['00', signatures], 'admin', 'error')
            if submit_method:
                self.record(submit_method, ['00', signatures], 'admin', 'error')

    def run_sidechain_payload_matrix(self, owner_address):
        owner_sig = 'aa' * 10
        committee_sigs = ['abcd', '0102']

        def base(seed):
            return {
                'sidechain_id': hash_hex(seed),
                'owner_address': owner_address,
                'validate': False,
            }

        issue = {
            'genesis_hash': hash_hex(100),
            'owner_address': owner_address,
            'token_symbol': 'SMK',
            'multiplier': 100,
            'fee_bps': 25,
            'max_multiplier': 100,
            'committee_m': 1,
            'committee_members': [owner_address],
            'checkpoint_policy_id': 2,
            'checkpoint_version': 2,
            'validate': False,
        }
        self.record_owner_signed_sidechain_flow(
            'createsidechainissuepayload',
            'finalizesidechainissue',
            'finalizesidechainissueandsubmit',
            issue,
            owner_sig,
        )

        lock = base(101)
        lock.update({
            'amount': 1000,
            'unlock_height': 10,
            'inputs': [tx_input(102)],
            'outputs': [tx_output(owner_address, 900)],
        })
        self.record_owner_signed_sidechain_flow(
            'createsidechainlockpayload',
            'finalizesidechainlock',
            None,
            lock,
            owner_sig,
        )

        spend = base(103)
        spend.update({
            'amount': 500,
            'outputs': [tx_output(owner_address, 500)],
        })
        self.record_owner_signed_sidechain_flow(
            'createsidechainspendpayload',
            'finalizesidechainspend',
            None,
            spend,
            owner_sig,
        )

        unlock = base(104)
        unlock['outputs'] = [tx_output(owner_address, 1000)]
        self.record_owner_signed_sidechain_flow(
            'createsidechainunlockpayload',
            'finalizesidechainunlock',
            None,
            unlock,
            owner_sig,
        )

        decommission = base(105)
        self.record_decommission_sidechain_flow(
            'createsidechaindecommissionpayload',
            'finalizesidechaindecommission',
            'finalizesidechaindecommissionandsubmit',
            decommission,
            owner_sig,
            committee_sigs,
        )

        mint = base(106)
        mint.update({
            'amount': 1000,
            'checkpoint': hash_hex(107),
            'committee_id': hash_hex(108),
        })
        self.record_committee_signed_sidechain_flow(
            'createsidechainmintpayload',
            'finalizesidechainmint',
            'finalizesidechainmintandsubmit',
            mint,
            committee_sigs,
        )

        redeem = base(109)
        redeem.update({
            'amount': 500,
            'checkpoint': hash_hex(110),
            'committee_id': hash_hex(111),
            'outputs': [tx_output(owner_address, 500)],
        })
        self.record_committee_signed_sidechain_flow(
            'createsidechainredeempayload',
            'finalizesidechainredeem',
            'finalizesidechainredeemandsubmit',
            redeem,
            committee_sigs,
        )

        slash = base(112)
        slash.update({
            'amount': 500,
            'outputs': [tx_output(owner_address, 400)],
            'committee_id': hash_hex(115),
            'checkpoint': hash_hex(116),
        })
        self.record_committee_signed_sidechain_flow(
            'createsidechainslashpayload',
            'finalizesidechainslash',
            'finalizesidechainslashandsubmit',
            slash,
            committee_sigs,
        )

        checkpoint = base(117)
        checkpoint.update({
            'checkpoint': hash_hex(118),
            'checkpoint_height': 1,
            'fee': 100,
            'committee_id': hash_hex(119),
        })
        self.record_committee_signed_sidechain_flow(
            'createsidechaincheckpointpayload',
            'finalizesidechaincheckpoint',
            None,
            checkpoint,
            committee_sigs,
        )

        batch = {
            'validate': False,
            'items': [{
                'sidechain_id': hash_hex(120),
                'checkpoint': hash_hex(121),
                'checkpoint_height': 1,
                'fee': 100,
                'committee_id': hash_hex(122),
                'version': 2,
                'proof_hex': 'abcd',
            }],
        }
        batch_created = self.result_of(self.record(
            'createbatchsidechaincheckpointpayload',
            [batch],
            'admin',
            'success',
        )) or {}
        batch_tx_hex = batch_created.get('tx_hex') if isinstance(batch_created, dict) else ''
        if batch_tx_hex:
            self.record('finalizebatchsidechaincheckpoint', [batch_tx_hex, [committee_sigs]], 'admin', 'success')
            self.record(
                'finalizebatchsidechaincheckpointandsubmit',
                [batch_tx_hex, [committee_sigs]],
                'admin',
                'error',
                allowed={-25, -26, -27},
            )
        else:
            self.record('finalizebatchsidechaincheckpoint', ['00', [committee_sigs]], 'admin', 'error')
            self.record('finalizebatchsidechaincheckpointandsubmit', ['00', [committee_sigs]], 'admin', 'error')

        committee = base(123)
        committee.update({
            'committee_m': 1,
            'committee_members': [owner_address],
        })
        self.record_committee_signed_sidechain_flow(
            'createsidechaincommitteeupdatepayload',
            'finalizesidechaincommitteeupdate',
            'finalizesidechaincommitteeupdateandsubmit',
            committee,
            committee_sigs,
        )

        self.record('createcheckpointproofv2', [{
            'sidechain_id': hash_hex(124),
            'state_root': hash_hex(125),
            'checkpoint_height': 1,
            'total_supply': 1000,
            'leaf_index': 0,
            'merkle_path': [],
            'verify': False,
        }], 'admin', 'success')
        self.record('createaccountingcheckpointproofv1', [{
            'sidechain_id': hash_hex(126),
            'state_root': hash_hex(127),
            'checkpoint_height': 1,
            'schema_hash': hash_hex(128),
        }], 'admin', 'success')

    def run_matrix(self):
        info = self.result_of(self.record('getblockchaininfo', [], 'ro', 'success')) or {}
        readonly_info = self.result_of(self.record('getrpcinfo', [], 'ro', 'success')) or {}
        rpc_info = self.result_of(self.record('getrpcinfo', [], 'admin', 'success')) or {}
        readonly_methods = {
            item.get('name') for item in readonly_info.get('commands', [])
            if isinstance(item, dict) and item.get('name')
        }
        expected_readonly_methods = {
            item.get('name') for item in rpc_info.get('commands', [])
            if isinstance(item, dict) and item.get('name') and item.get('readonly') is True
        }
        self.record_check(
            'rpc_registry_readonly_scope',
            bool(readonly_methods) and readonly_methods == expected_readonly_methods,
            'readonly registry must expose exactly the commands authorized for readonly',
            {'expected': sorted(expected_readonly_methods), 'actual': sorted(readonly_methods)},
        )
        live_methods = sorted({
            item.get('name') for item in rpc_info.get('commands', [])
            if isinstance(item, dict) and item.get('name')
        })
        if live_methods:
            expected = set(self.expected_methods)
            live = set(live_methods)
            self.ctx['rpc_registry'] = {
                'source_methods': len(self.source_methods),
                'fallback_methods': len(ALL_REGISTERED),
                'expected_methods': len(self.expected_methods),
                'live_methods': len(live_methods),
                'missing_live_methods': sorted(expected - live),
                'unexpected_live_methods': sorted(live - expected),
            }
            self.record_check(
                'rpc_registry_source_match',
                expected == live,
                'live getrpcinfo command set must match registered C++ RPC names',
                self.ctx['rpc_registry'],
            )
        height = int(info.get('blocks') or 0)
        best_hash = info.get('bestblockhash') or ''
        self.ctx['height'] = height
        self.ctx['best_hash'] = best_hash

        best_hash = self.result_of(self.record('getblockhash', [height], 'ro', 'success')) or best_hash
        block = self.result_of(self.record('getblock', [best_hash, True], 'ro', 'success')) or {}
        self.record('getblockbyheight', [height, True], 'ro', 'success')
        geography_page = self.result_of(self.record(
            'listblockgeography', [height, 1], 'ro', 'success',
        )) or []
        self.record_check(
            'block_geography_tip_matches',
            len(geography_page) == 1 and geography_page[0].get('hash') == best_hash,
            'geography catalogue must identify the same active block',
            geography_page,
        )
        geometry = {'parts': [{'exterior': [
            {'lat_ndeg': 0, 'lon_ndeg': 0},
            {'lat_ndeg': 0, 'lon_ndeg': 1000000},
            {'lat_ndeg': 1000000, 'lon_ndeg': 0},
        ], 'holes': []}]}
        analysis = self.result_of(self.record(
            'analyzegeometry', [geometry, 1], 'ro', 'success',
        )) or {}
        self.record_check(
            'geometry_analysis_contract',
            analysis.get('valid') is True and analysis.get('rules_version') == 1 and
            analysis.get('geojson', {}).get('type') == 'Polygon' and
            analysis.get('area_sqcm', 0) > 0,
            'geometry analysis must return valid version-one geometry and area',
            analysis,
        )
        self.record('getnftgeojson', ['0' * 64], 'ro', 'error', allowed={-8})
        first_txid = (block.get('tx') or ['0' * 64])[0]
        raw_tx = self.result_of(self.record('getrawtransaction', [first_txid, 1], 'ro', 'success')) or {}
        chain_addr = 'GEO1KrNQJ6C6qYNH4N2goZPU2eDAfiPvknY2x'
        outputs = raw_tx.get('outputs') or []
        if outputs and outputs[0].get('address'):
            chain_addr = outputs[0]['address']
        self.ctx.update({'txid': first_txid, 'chain_addr': chain_addr})

        created_wallet = self.result_of(self.record('createwallet', [''], 'admin', 'success')) or {}
        wallet_addr = created_wallet.get('address') or chain_addr
        wallet_addr = self.result_of(self.record('getnewaddress', ['apitest'], 'admin', 'success')) or wallet_addr
        self.ctx['wallet_addr'] = wallet_addr
        self.record('dumpprivkey', [wallet_addr], 'admin', 'success')
        self.record(
            'importprivkey',
            ['0000000000000000000000000000000000000000000000000000000000000002', 'apitest-import'],
            'admin',
            'success_or_error',
            allowed={-4, -8},
        )

        sig_result = self.result_of(self.record('signmessage', [wallet_addr, 'api-smoke-message'], 'admin', 'success'))
        if isinstance(sig_result, dict):
            signature = sig_result.get('signature') or ''
        else:
            signature = sig_result or ''
        self.ctx['signature_len'] = len(signature)
        self.record('verifysignature', [wallet_addr, signature, 'api-smoke-message'], 'ro', 'success' if signature else 'error')

        zero = '0' * 64
        self.record('getblockcount', [], 'ro', 'success')
        self.record('getblockrejectstats', [], 'ro', 'success')
        self.record('getwitnesspubkeystats', [], 'ro', 'success')
        self.record('getinterestpool', [], 'ro', 'success')
        self.record('getprunedrange', [], 'ro', 'success')
        self.record('getblockavailability', [height], 'ro', 'success')
        self.record('getavailableblocks', [max(0, height - 2), 3], 'ro', 'success')
        self.record('getsidechaingenesis', [zero], 'ro', 'error', allowed={-8, -5, -1})
        self.record('createsidechaingenesis', [{}], 'admin', 'success')
        self.record('getblockchildren', [best_hash], 'ro', 'success')
        self.record('getblocktree', [best_hash, 2], 'ro', 'success')
        self.record('switchtotip', [best_hash], 'admin', 'success')
        self.record('prunenow', [], 'admin', 'error', allowed={-32600})
        self.record('unprune', [height], 'admin', 'success_or_error', allowed={-8, -1})
        self.record('getdelegationproof', [first_txid], 'ro', 'error', allowed={-8, -5, -1})
        self.record('gettxout', [first_txid, 0], 'ro', 'success')
        self.record('getaddressbalance', [chain_addr], 'ro', 'success')
        self.record('getaddresstxids', [chain_addr], 'ro', 'success')
        self.record('getaddresshistory', [chain_addr], 'ro', 'success')

        self.record('getmininginfo', [], 'miner', 'success')
        self.record('getmempoolinfo', [], 'miner', 'success')
        self.record('getrawmempool', [], 'ro', 'success')
        self.record('getmempoolentry', [zero], 'ro', 'error', allowed={-5, -8})
        self.record('testmempoolaccept', ['00'], 'ro', 'error', allowed={-8, -22, -25, -26})
        self.record('getnetworkhashps', [], 'miner', 'success')
        self.record('setgenerate', [False], 'admin', 'success')
        self.record('getblocktemplate', [{'address': wallet_addr}], 'miner', 'success')
        self.record('submitblock', ['00'], 'miner', 'error', allowed={-22, -8})

        self.record('getpeerinfo', [], 'admin', 'success')
        self.record('getconnectioncount', [], 'ro', 'success')
        self.record('addnode', ['127.0.0.1:39999', 'add'], 'admin', 'success_or_error', allowed={-23, -8, -1})
        self.record('getaddednodeinfo', [], 'admin', 'success')
        self.record('getp2ppolicystats', [], 'ro', 'success')
        self.record('getconnectiondiagnostics', [], 'admin', 'success')
        self.record('disconnectnode', ['127.0.0.1:39999'], 'admin', 'success_or_error', allowed={-29, -8, -1})
        self.record('setban', ['203.0.113.9', 'add', 60], 'admin', 'success_or_error', allowed={-8, -1})
        self.record('listbans', [], 'admin', 'success')
        self.record('clearbans', [], 'admin', 'success')
        try:
            self.rpc('addnode', ['127.0.0.1:39999', 'remove'], role='admin')
        except Exception:
            pass

        self.record('help', [], 'ro', 'success')
        self.record('powdeterminism', [], 'ro', 'success')
        self.record('getmetrics', [], 'ro', 'success')
        self.record('validateaddress', [wallet_addr], 'ro', 'success')
        self.record('getwalletaddressinfo', [wallet_addr], 'admin', 'success')
        self.record('getinfo', [], 'ro', 'success')
        self.record('getnodeinfo', [], 'ro', 'success')
        self.record('getsyncstatus', [], 'ro', 'success')
        self.record('getstorageinfo', [], 'admin', 'success')
        self.record('getdbstats', [], 'admin', 'success')
        self.record('getpruninginfo', [], 'ro', 'success')
        self.record('gethealth', [], 'ro', 'success')
        self.record('getfalconinfo', [], 'ro', 'success')

        self.record('getbalance', [], 'admin', 'success')
        self.record('listaddressbalances', [], 'admin', 'success')
        self.record('listunspent', [0, 9999999], 'admin', 'success')
        self.record('getaddressutxos', [chain_addr], 'ro', 'success')
        self.record('getfeerate', [], 'ro', 'success')
        self.record('sendtoaddress', [wallet_addr, 0.01], 'admin', 'success_or_error', allowed={-4, -6})
        self.record('sendrawtransaction', ['00'], 'admin', 'error', allowed={-8, -22, -25, -26})
        self.record('verifyrawtransaction', ['00'], 'ro', 'error', allowed={-8, -22, -25, -26})
        raw_created = self.result_of(self.record(
            'createrawtransaction',
            [[{'txid': zero, 'vout': 0}], [{'address': wallet_addr, 'amount': 0.01}]],
            'ro',
            'success',
        ))
        if raw_created:
            self.record('decoderawtransaction', [raw_created], 'ro', 'success')
            self.record('signrawtransactionwithwallet', [raw_created], 'admin', 'success')
        else:
            self.record('decoderawtransaction', ['00'], 'ro', 'error', allowed={-8, -22})
            self.record('signrawtransactionwithwallet', ['00'], 'admin', 'error', allowed={-8, -22})
        self.record('getnft', [zero], 'ro', 'success')
        self.record('listnftsbyowner', [wallet_addr], 'ro', 'success')
        self.record('getnftshistory', [zero], 'ro', 'success')
        self.record('getutxos', [chain_addr], 'ro', 'success')
        self.record('createnft', [{}], 'admin', 'error')
        self.record('transfernft', [{}], 'admin', 'error')
        self.record('burnnft', [{}], 'admin', 'error')

        for method in ['createnftcreatepayload', 'createnfttransferpayload', 'createnftburnpayload']:
            self.record(method, [{}], 'admin', 'error')
        for method in ['finalizenftcreate', 'finalizenfttransfer', 'finalizenftburn']:
            self.record(method, ['00', '00'], 'admin', 'error')

        self.run_sidechain_payload_matrix(wallet_addr)

        for method in [
            'createsidechainissuepayload', 'createaccountingsidechainpayload',
            'createsidechainmintpayload', 'createsidechainredeempayload',
            'createsidechainslashpayload', 'createsidechainlockpayload', 'createsidechainspendpayload',
            'createsidechaintopuppayload', 'createsidechainunlockpayload',
            'createsidechaincheckpointpayload', 'createcheckpointproofv2',
            'createaccountingcheckpointproofv1',
            'createbatchsidechaincheckpointpayload', 'createsidechaincommitteeupdatepayload',
            'createsidechaindecommissionpayload', 'sendmultisigpackage',
            'createdelegationproofdraft', 'signanglecommitment', 'finalizedelegationproof',
            'createhierarchicalspenddraft', 'finalizehierarchicalspend', 'createdelegationproof',
            'createrevokedelegation', 'createrevokedelegationdraft',
            'senddelegationpackage', 'sendrevokepackage',
        ]:
            self.record(method, [{}], 'admin', 'error')
        for method in [
            'finalizesidechainissue', 'finalizesidechainissueandsubmit',
            'finalizeaccountingsidechain', 'finalizeaccountingsidechainandsubmit',
            'finalizesidechainlock', 'finalizesidechaintopup',
            'finalizesidechainspend', 'finalizesidechainunlock',
            'finalizesidechaindecommission', 'finalizesidechaindecommissionandsubmit',
        ]:
            self.record(method, ['00', '00'], 'admin', 'error')
        for method in [
            'finalizesidechainmint', 'finalizesidechainmintandsubmit',
            'finalizesidechainredeem', 'finalizesidechainredeemandsubmit',
            'finalizesidechainslash', 'finalizesidechainslashandsubmit',
            'finalizesidechaincheckpoint', 'finalizebatchsidechaincheckpoint',
            'finalizebatchsidechaincheckpointandsubmit',
            'finalizesidechaincommitteeupdate', 'finalizesidechaincommitteeupdateandsubmit',
        ]:
            self.record(method, ['00', []], 'admin', 'error')

        self.record('listtransactions', ['*', 10, 0], 'admin', 'success')
        self.record('getaddressesbyaccount', ['*'], 'admin', 'success')
        self.record('getfundbalance', [], 'ro', 'success')
        self.record('registermultisig', [wallet_addr, 1, 1, wallet_addr], 'admin', 'success_or_error', allowed={-4, -8, -5})
        self.record('stop', [], 'admin', 'success')

    def wait_stopped(self):
        if not self.process:
            return False
        try:
            self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            return False
        try:
            self.rpc('getblockcount', [], role='ro', timeout=2)
            return False
        except Exception:
            return True

    def write_results(self):
        missing = sorted(set(self.expected_methods) - self.seen)
        failures = [item for item in self.results if not item.get('ok')]
        summary = {
            'context': self.ctx,
            'registered_methods_expected': len(self.expected_methods),
            'registered_methods_from_source': len(self.source_methods),
            'unique_methods_seen': len(self.seen),
            'missing_methods': missing,
            'total_calls_recorded': len(self.results),
            'passed': len(self.results) - len(failures),
            'failed': len(failures),
            'failures': failures,
        }
        out = {'summary': summary, 'results': self.results}
        result_path = Path(self.args.data_dir) / 'rpc-smoke-results.json'
        result_path.write_text(json.dumps(out, ensure_ascii=False, indent=2), encoding='utf-8')
        return summary, result_path

    def stop_node_forcefully(self):
        if not self.process or self.process.poll() is not None:
            return
        if os.name != 'nt':
            try:
                os.killpg(os.getpgid(self.process.pid), signal.SIGTERM)
            except ProcessLookupError:
                return
        else:
            self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            if os.name != 'nt':
                os.killpg(os.getpgid(self.process.pid), signal.SIGKILL)
            else:
                self.process.kill()
            self.process.wait(timeout=10)

    def run(self):
        try:
            self.start_node()
            self.wait_rpc_and_sync()
            self.run_matrix()
            stopped = self.wait_stopped()
            if not stopped:
                self.results.append({
                    'method': 'stop_effect',
                    'role': 'admin',
                    'expect': 'process_stopped',
                    'ok': False,
                    'why': 'RPC stop returned but node did not stop',
                })
            summary, result_path = self.write_results()
            failures = summary['failures']
            print(json.dumps({
                'data_dir': self.args.data_dir,
                'result_path': str(result_path),
                'context': summary['context'],
                'registered_methods_expected': summary['registered_methods_expected'],
                'unique_methods_seen': summary['unique_methods_seen'],
                'total_calls_recorded': summary['total_calls_recorded'],
                'passed': summary['passed'],
                'failed': summary['failed'],
                'missing_methods': summary['missing_methods'],
            }, ensure_ascii=False, indent=2))
            if failures or summary['missing_methods']:
                for item in failures[:20]:
                    print('FAIL', json.dumps({
                        key: item.get(key) for key in ['method', 'role', 'expect', 'code', 'why', 'transport_error']
                    }, ensure_ascii=False))
                return 1
            return 0
        finally:
            self.stop_node_forcefully()
            if self.log_file:
                self.log_file.close()


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]

def parse_args(argv):
    parser = argparse.ArgumentParser(description='GeoCoin JSON-RPC integration smoke test')
    parser.add_argument('--node-bin', required=True)
    parser.add_argument('--network', default='degree')
    parser.add_argument('--data-dir')
    parser.add_argument('--p2p-port', type=int, default=0)
    parser.add_argument('--rpc-port', type=int, default=0)
    parser.add_argument('--sync-timeout', type=int, default=180)
    parser.add_argument('--min-height', type=int, default=1)
    parser.add_argument('--clean-data-dir', action='store_true')
    parser.add_argument('--keep-data-dir', action='store_true')
    parser.add_argument('--addnode', action='append')
    parser.add_argument('--no-default-addnodes', action='store_true')
    args = parser.parse_args(argv)
    default_addnodes = [
        'node1.geocoin.network:3030',
        'node2.geocoin.network:3030',
        'node3.geocoin.network:3030',
    ]
    if args.addnode is None:
        args.addnode = [] if args.no_default_addnodes else default_addnodes
    elif not args.no_default_addnodes:
        args.addnode = default_addnodes + args.addnode
    if args.p2p_port == 0:
        args.p2p_port = find_free_port()
    if args.rpc_port == 0:
        args.rpc_port = find_free_port()
    if args.data_dir is None:
        args.data_dir = tempfile.mkdtemp(prefix='geocoin-rpc-smoke-')
        args.clean_data_dir = False
    return args

def main(argv):
    args = parse_args(argv)
    data_dir = Path(args.data_dir)
    rc = 1
    try:
        rc = RpcSmoke(args).run()
        return rc
    finally:
        if rc == 0 and not args.keep_data_dir and data_dir.exists() and 'geocoin-rpc-smoke-' in data_dir.name:
            shutil.rmtree(data_dir, ignore_errors=True)

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
