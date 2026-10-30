#!/usr/bin/env groovy
library 'status-jenkins-lib@v1.10.3'

pipeline {
    agent { label 'windows && x86_64' }

    parameters {
        string(name: 'BUILD_JOB', description: 'Job that produced the artifact')
        string(name: 'BUILD_NUMBER', description: 'Build number to sign')
    }

    options {
        timestamps()
        ansiColor('xterm')
        timeout(time: 30, unit: 'MINUTES')
        buildDiscarder(logRotator(numToKeepStr: '10', daysToKeepStr: '30', artifactNumToKeepStr: '1'))
    }

    stages {
        stage('Cleanup') {
            steps {
                cleanWs()
                checkout scm
            }
        }
        stage('Copy Artifact') {
            steps {
                copyArtifacts(
                    projectName: params.BUILD_JOB,
                    selector: specific(params.BUILD_NUMBER),
                    filter: 'pkg/*.zip',
                    flatten: false
                )
            }
        }
        stage('Sign') {
            steps {
                withCredentials([
                    string(credentialsId: 'logos-keylocker-api-key', variable: 'SM_API_KEY'),
                    string(credentialsId: 'logos-keylocker-cert-pass', variable: 'SM_CLIENT_CERT_PASSWORD'),
                    string(credentialsId: 'logos-keylocker-keypair-alias', variable: 'WINDOWS_DIGICERT_KEYPAIR_ALIAS'),
                    file(credentialsId: 'logos-keylocker-auth-cert', variable: 'SM_CLIENT_CERT_FILE'),
                ]) {
                    sh './scripts/windows-sign.sh'
                }
            }
        }
        stage('Upload') {
            steps { script {
                env.PKG_URL = s5cmd.upload(findFiles(glob: 'pkg/*.zip')[0].path)
                jenkins.setBuildDesc(Signed: env.PKG_URL)
            } }
        }
        stage('Archive') {
            steps { archiveArtifacts('pkg/*.zip') }
        }
    }

    post {
        cleanup {
            cleanWs(deleteDirs: true, disableDeferredWipeout: true)
            dir("${env.WORKSPACE}@tmp") { deleteDir() }
            dir("${env.WORKSPACE}@script") { deleteDir() }
        }
    }
}