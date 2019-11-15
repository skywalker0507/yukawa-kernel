#!/bin/sh

set -xe

JOB_IDS=""
RESULT=0

for job in $1/* ; do
    lava_job_id=`lavacli jobs submit $job`
    jobname=`basename $job .yaml`
    echo $jobname: $lava_job_id | tee -a $2/lava-jobs.txt
    eval "job_$lava_job_id='$jobname'"
    JOB_IDS="$JOB_IDS $lava_job_id"
done

for job in $JOB_IDS ; do
    jobname=`eval echo '$'job_$job`
    echo Waiting for $job ($jobname)
    lavacli jobs wait $job
done

for job in $JOB_IDS ; do
    jobname=`eval echo '$'job_$job`
    echo Result for $job ($jobname)
    lavacli jobs logs $job | grep -a -v "{'case':" > $2/$jobname.log
    lavacli jobs show $lava_job_id
    lavacli results $lava_job_id
    status=`lavacli jobs show $lava_job_id | grep -c Finished` || echo Failed
    echo status $status
    [ "$status" -gt 0 ] && RESULT=1
    fails=`lavacli results $lava_job_id | grep -c fail` || echo Success
    echo fails $fails
    [ "$fails" -eq 0 ] && RESULT=1
done

exit $RESULT
