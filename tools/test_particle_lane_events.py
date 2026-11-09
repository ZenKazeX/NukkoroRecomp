import copy
import unittest
from tools.particle_lane_events import analyze

def fixture():
    event=dict(sequence=1,instructions=6000000019,site='0x8044a25c',lr='0x8044a240',
        vi=367,guest_frame=0,
        context_fields_observed=0,current_context='0x00000000',fp_owner='0x00000000',thread='0x00000000',
        f18_f19_raw_u64=['0xFFF8000000000001','0x8000000000000000','0x0000000000000000','0xFFFFFFFFFFFFFFFF'],
        roots=['0x00000000']*12,gqr=['0x00000000']*8,msr='0x00009032',fpscr='0x00000000')
    return dict(ordered_numeric_events=dict(schema=1,entry_observations_only=True,capacity=2,retained_bytes=352,
        hits=1,stored=1,dropped=0,counter_overflow=False,complete_for_observed_entries=True,
        root_registers=[1,3,4,5,6,7,13,27,28,29,30,31],lane_order=['f18.0','f18.1','f19.0','f19.1'],events=[event]))

class OrderedEventsTests(unittest.TestCase):
    def test_raw_bits_and_unobserved_zero_not_supplied_context(self):
        report=analyze(fixture());row=report['first_five_observed_entries'][0]
        self.assertEqual(row['context'],(None,None,None));self.assertEqual(row['raw_lanes'][0],'0xFFF8000000000001')
        self.assertFalse(report['native_retirement_proven']);self.assertFalse(report['producer_output_observed'])
    def test_counter_or_completeness_contradictions_rejected(self):
        for key,value in (('hits',2),('complete_for_observed_entries',False),('stored',2),('capacity',0)):
            report=fixture();report['ordered_numeric_events'][key]=value
            with self.assertRaises(ValueError):analyze(report)
    def test_drops_are_incomplete_not_wrap(self):
        doc=fixture();trace=doc['ordered_numeric_events'];trace.update(hits=4,dropped=3,complete_for_observed_entries=False)
        self.assertEqual(analyze(doc)['dropped'],3)
    def test_raw_numeric_lanes_refused(self):
        doc=fixture();doc['ordered_numeric_events']['events'][0]['f18_f19_raw_u64'][0]=0xfff8000000000001
        with self.assertRaises(ValueError):analyze(doc)
    def test_reordered_prefix_refused(self):
        doc=fixture();doc['ordered_numeric_events']['events'][0]['sequence']=2
        with self.assertRaises(ValueError):analyze(doc)
    def test_transitions_never_claim_causality_or_ignore_batched_coordinates(self):
        doc=fixture();trace=doc['ordered_numeric_events'];event=copy.deepcopy(trace['events'][0]);event.update(sequence=2,instructions=1,context_fields_observed=7)
        event['f18_f19_raw_u64'][3]='0x0000000000000001';trace['events'].append(event);trace.update(stored=2,hits=2)
        report=analyze(doc);self.assertEqual(report['instruction_coordinate_regressions'],[2])
        self.assertEqual(report['adjacent_observation_transitions'][0]['changed_lanes'],['f19.1'])
        self.assertTrue(report['adjacent_observation_transitions'][0]['context_changed'])
        self.assertFalse(report['adjacent_observation_transitions'][0]['causal_owner_proven'])
    def test_global_measure_wrapper(self):
        self.assertEqual(analyze({'r5probes':fixture()})['stored'],1)
    def test_installation_and_population_are_checked_independently(self):
        doc=fixture();doc['config']=dict(numeric_entries=['0x8044a25c'])
        doc['probes']=[dict(address='0x8044a25c',hits=1,installed=True,
            numeric_entry_evidence=dict(hits=1,states=[dict(hits=1)],unclassified=0))]
        self.assertTrue(analyze(doc)['configured_numeric_installation_verified'])
        doc['probes'][0]['installed']=False
        self.assertFalse(analyze(doc)['configured_numeric_installation_verified'])
        doc['probes'][0]['hits']=2
        with self.assertRaises(ValueError):analyze(doc)
    def test_retained_capacity_is_exact_frozen_event_size(self):
        doc=fixture();doc['ordered_numeric_events']['retained_bytes']=368
        with self.assertRaises(ValueError):analyze(doc)
    def test_u64_fields_and_frames_reject_bool_negative_and_overflow(self):
        for key in ('instructions','vi','guest_frame'):
            for value in (True,-1,1<<64):
                doc=fixture();doc['ordered_numeric_events']['events'][0][key]=value
                with self.assertRaises(ValueError):analyze(doc)
        for value in (True,-1,1<<64):
            doc=fixture();doc['ordered_numeric_events']['hits']=value
            with self.assertRaises(ValueError):analyze(doc)
    def test_disabled_log_cannot_claim_activity_or_completeness(self):
        doc=fixture();trace=doc['ordered_numeric_events'];trace.update(capacity=0,retained_bytes=0,stored=0,events=[],hits=1,dropped=1,complete_for_observed_entries=False)
        with self.assertRaises(ValueError):analyze(doc)
        trace.update(hits=0,dropped=0)
        self.assertFalse(analyze(doc)['complete_for_observed_entries'])
    def test_duplicate_unknown_sites_and_false_full_installation_claim_rejected(self):
        base=fixture();base['config']=dict(numeric_entries=['0x8044a25c'])
        base['probes']=[dict(address='0x8044a25c',hits=1,installed=True,
            numeric_entry_evidence=dict(hits=1,states=[dict(hits=1)],unclassified=0))]
        doc=copy.deepcopy(base);doc['config']['numeric_entries'].append('0x8044A25C')
        with self.assertRaises(ValueError):analyze(doc)
        doc=copy.deepcopy(base);doc['probes'].append(copy.deepcopy(doc['probes'][0]))
        with self.assertRaises(ValueError):analyze(doc)
        doc=copy.deepcopy(base);doc['ordered_numeric_events']['events'][0]['site']='0x8044a620'
        with self.assertRaisesRegex(ValueError,'stored event site'):analyze(doc)
        doc=copy.deepcopy(base);doc['probes'][0]['installed']=False
        doc['ordered_numeric_events']['whole_route_coverage_proven']=True
        with self.assertRaisesRegex(ValueError,'declared full completeness'):analyze(doc)
    def test_numeric_evidence_counters_are_exact_u64(self):
        base=fixture();base['config']=dict(numeric_entries=['0x8044a25c'])
        base['probes']=[dict(address='0x8044a25c',hits=1,installed=True,
            numeric_entry_evidence=dict(hits=1,states=[dict(hits=1)],unclassified=0))]
        for target in ('probe','evidence','state','unclassified'):
            for value in (True,-1,1<<64):
                doc=copy.deepcopy(base);p=doc['probes'][0]
                if target=='probe':p['hits']=value
                elif target=='evidence':p['numeric_entry_evidence']['hits']=value
                elif target=='state':p['numeric_entry_evidence']['states'][0]['hits']=value
                else:p['numeric_entry_evidence']['unclassified']=value
                with self.assertRaises(ValueError):analyze(doc)

if __name__=='__main__':unittest.main()
