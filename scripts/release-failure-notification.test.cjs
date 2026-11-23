const test = require('node:test');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const assert = require('node:assert/strict');
const releaseFailureNotification = require('./release-failure-notification.cjs');

const publishName = 'Commit and publish approved release';
const resultName = 'Notify Slack/PostHog - Release Result';

const job = (name, conclusion, steps = [{ name: 'Set up job', conclusion: 'success' }]) => ({
    name,
    conclusion,
    steps,
    html_url: `https://github.com/PostHog/posthog-unreal/actions/runs/1/job/${name.length}`,
});

const step = (name, conclusion) => ({ name, conclusion });

async function run({ conclusion, jobs, threadDirectory, attempt = 1 }) {
    const github = {
        paginate: async () => jobs,
        rest: { actions: { listJobsForWorkflowRunAttempt: () => {} } },
    };
    const context = {
        repo: { owner: 'PostHog', repo: 'posthog-unreal' },
        payload: {
            workflow_run: {
                id: 1,
                run_attempt: attempt,
                conclusion,
                html_url: 'https://github.com/PostHog/posthog-unreal/actions/runs/1',
            },
        },
    };
    return releaseFailureNotification({ github, context, channel: 'C123', threadDirectory });
}

test('stays quiet when the run succeeded', async () => {
    const result = await run({
        conclusion: 'success',
        jobs: [
            job(publishName, 'success'),
            job(resultName, 'success', [step('Notify Slack - Released', 'success')]),
        ],
    });
    assert.equal(result, null);
});

test('alerts when a job failed', async () => {
    const result = await run({
        conclusion: 'failure',
        jobs: [job(publishName, 'failure'), job(resultName, 'success')],
    });
    assert.equal(result.channel, 'C123');
    assert.match(result.text, /Release process failed or was cancelled/);
    assert.match(result.text, new RegExp(`\\|${publishName}>: failure`));
});

test('stays quiet when the approval was rejected and the reply was delivered', async () => {
    const result = await run({
        conclusion: 'failure',
        jobs: [
            job(publishName, 'failure', []),
            job(resultName, 'success', [step('Notify Slack - Rejected', 'success')]),
        ],
    });
    assert.equal(result, null);
});

test('stays quiet for a cancelled run with no jobs', async () => {
    const result = await run({ conclusion: 'cancelled', jobs: [] });
    assert.equal(result, null);
});

test('alerts when the release exists but the released notice was skipped', async () => {
    const result = await run({
        conclusion: 'success',
        jobs: [job(publishName, 'success'), job(resultName, 'success', [step('Notify Slack - Released', 'skipped')])],
    });
    assert.match(result.text, /GitHub release published; follow-up problems/);
    assert.match(result.text, /released Slack notification did not run/);
});

const failedRun = { conclusion: 'failure', jobs: [job(publishName, 'failure'), job(resultName, 'success')] };

function threadDirectory(layout) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'thread-'));
    for (const [file, value] of Object.entries(layout)) {
        fs.mkdirSync(path.dirname(path.join(dir, file)), { recursive: true });
        fs.writeFileSync(path.join(dir, file), value);
    }
    return dir;
}

test('replies in the thread when a lone artifact is extracted flat', async () => {
    const dir = threadDirectory({ 'thread-ts.txt': '1700000000.123456\n' });
    const result = await run({ ...failedRun, threadDirectory: dir });
    assert.equal(result.thread_ts, '1700000000.123456');
});

test('picks the newest attempt not above the current one from subdirectories', async () => {
    const dir = threadDirectory({
        'release-slack-thread-1/thread-ts.txt': '1700000001.000001',
        'release-slack-thread-2/thread-ts.txt': '1700000002.000002',
        'release-slack-thread-3/thread-ts.txt': '1700000003.000003',
    });
    const result = await run({ ...failedRun, threadDirectory: dir, attempt: 2 });
    assert.equal(result.thread_ts, '1700000002.000002');
});
